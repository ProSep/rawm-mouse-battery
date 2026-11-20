// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID battery driver for Rawmtech / RAWM wireless mice
 * (Leviathan V4 and siblings, receiver USB id 1915:2346; the mouse itself
 * enumerates as 1915:234a while it is plugged in by cable).
 *
 * Exposes the mouse battery through the kernel power_supply subsystem so it
 * shows up in UPower, desktop battery widgets, etc.
 *
 * Vendor protocol (decoded from the RAWM web hub, library.min.js):
 *
 * Each 64-byte report on the vendor interface (usage page 0xFF00) is
 *     [0xC0 | ch]?  [type | size]  payload[size]
 * where a leading 0xC0|ch byte addresses ESB channel ch (0 = the mouse) and
 * its absence addresses the receiver itself; type 0x80 = data, 0x40 = flow
 * control. Payloads form a per-channel byte stream of messages:
 *     ff ff ff ff  [cmd | len_hi << 4] [len_lo] ...   (len includes header)
 *
 * We send CMD_QUERY to the mouse:
 *     c0 8d  01 0d 03 00 00 <unix seconds, u64 LE>
 * and the mouse answers with CMD_QUERY_RESULT (cmd 2) carrying JSON such as
 *     {"dn":"LEVIATHAN V4","battery":35,"chr":0,...}
 * The mouse may also push NOTIFY (cmd 0xb) type 0x17: body[0] = percent,
 * body[1] == 1 -> charging.
 *
 * Wired mode: with the cable in, the mouse stops answering over the radio
 * and instead speaks the same protocol on its own vendor interface, without
 * the 0xC0|ch channel byte. While any wired mouse is present, receivers
 * query more often and drop their battery as soon as a query goes
 * unanswered (that receiver's mouse is the one on the cable); a receiver
 * whose mouse still answers keeps its battery. Userspace thus sees one
 * battery per mouse, with live charging data while it is wired.
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/usb.h>

#define DRV_NAME		"hid-miracletek"

#define POLL_MS			60000
#define RETRY_MS		10000	/* while no battery is registered */
#define FIRST_POLL_MS		1000
#define UNPLUG_POLL_MS		3000	/* radio needs a moment to relink */
#define WIRED_CHECK_MS		5000	/* receiver poll while a mouse is wired */
#define VENDOR_IFACE_NUM	2

#define PID_RECEIVER		0x2346
#define PID_WIRED_MOUSE		0x234a
#define STALE_POLLS		5	/* no reply for this many polls -> unknown */
#define REPORT_LEN		64
#define STREAM_LEN		2048

#define CH_MOUSE		0
#define PKT_TYPE_MASK		0xc0
#define PKT_EXTENDED		0xc0
#define PKT_EVENT_DATA		0x80
#define PKT_SIZE_MASK		0x3f

#define CMD_QUERY		0x01
#define CMD_QUERY_RESULT	0x02
#define CMD_NOTIFY		0x0b
#define NOTIFY_BATTERY		0x17
#define OS_PC			0x03

struct miracletek_drvdata {
	struct hid_device *hdev;
	struct device *anchor;		/* parent of battery + type hint */
	struct input_dev *type_hint;	/* silent "mouse" for UPower */
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	struct delayed_work poll_work;
	struct work_struct register_work;
	struct mutex reg_mutex;		/* serializes register vs. destroy */
	struct list_head node;		/* on mt_receivers (receivers only) */
	bool wired;			/* 1915:234a: the mouse on a cable */
	bool opened;
	char anchor_name[48];
	char battery_name[64];
	char serial[32];

	spinlock_t lock;		/* protects everything below */
	bool removing;
	bool register_failed;		/* don't retry: avoids a retry storm */
	bool wired_check;		/* receiver: a mouse is wired somewhere */
	u8 stream[STREAM_LEN];
	size_t stream_len;
	int capacity;			/* -1 = never read */
	bool charging;
	unsigned int missed_polls;
	bool got_reply;
	char model[32];
};

/* Receiver instances, and how many wired mice are plugged in. */
static DEFINE_MUTEX(mt_list_lock);
static LIST_HEAD(mt_receivers);
static unsigned int mt_wired_count;

static enum power_supply_property miracletek_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static int miracletek_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct miracletek_drvdata *drv = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&drv->lock, flags);
	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		/* mouse off / asleep / out of range: don't show a stale % */
		val->intval = drv->missed_polls < STALE_POLLS;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (drv->missed_polls >= STALE_POLLS)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (drv->charging)
			val->intval = drv->capacity >= 100 ?
				      POWER_SUPPLY_STATUS_FULL :
				      POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = drv->capacity < 0 ? 0 : drv->capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = drv->model;	/* fixed once registered */
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "RAWM";
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = drv->serial;	/* fixed once registered */
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&drv->lock, flags);
	return ret;
}

/* ---- sending ---------------------------------------------------------- */

static int miracletek_send_query(struct miracletek_drvdata *drv)
{
	struct hid_device *hdev = drv->hdev;
	u64 ts = ktime_get_real_seconds();
	u8 *buf, *m;
	int i, ret;

	buf = kzalloc(REPORT_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (drv->wired) {		/* talking to the mouse directly */
		buf[0] = PKT_EVENT_DATA | 13;
		m = buf + 1;
	} else {			/* via the receiver, ESB channel 0 */
		buf[0] = PKT_EXTENDED | CH_MOUSE;
		buf[1] = PKT_EVENT_DATA | 13;
		m = buf + 2;
	}
	m[0] = CMD_QUERY;		/* | (len >> 4 & 0xf0) == 0 */
	m[1] = 13;			/* message length */
	m[2] = OS_PC;
	m[3] = 0;
	m[4] = 0;
	for (i = 0; i < 8; i++)
		m[5 + i] = (ts >> (8 * i)) & 0xff;

	ret = hid_hw_output_report(hdev, buf, REPORT_LEN);
	kfree(buf);
	return ret < 0 ? ret : 0;
}

/* ---- receiving -------------------------------------------------------- */

static const u8 *find_bytes(const u8 *hay, size_t hlen,
			    const char *needle, size_t nlen)
{
	size_t i;

	if (hlen < nlen)
		return NULL;
	for (i = 0; i + nlen <= hlen; i++)
		if (!memcmp(hay + i, needle, nlen))
			return hay + i;
	return NULL;
}

/* Parse the integer after "key": in a non-terminated JSON blob. */
static bool json_int(const u8 *js, size_t len, const char *key, int *out)
{
	const u8 *p = find_bytes(js, len, key, strlen(key));
	const u8 *end = js + len;
	int v = 0, digits = 0;

	if (!p)
		return false;
	p += strlen(key);
	while (p < end && *p >= '0' && *p <= '9' && digits < 6) {
		v = v * 10 + (*p - '0');
		p++;
		digits++;
	}
	if (!digits)
		return false;
	*out = v;
	return true;
}

/* Copy the string value of "key":"..." into out (printable ASCII only). */
static void json_str(const u8 *js, size_t len, const char *key,
		     char *out, size_t outlen)
{
	const u8 *p = find_bytes(js, len, key, strlen(key));
	const u8 *end = js + len;
	size_t n = 0;

	if (!p)
		return;
	p += strlen(key);
	while (p < end && *p != '"' && n + 1 < outlen) {
		out[n++] = (*p >= 0x20 && *p < 0x7f) ? *p : '?';
		p++;
	}
	out[n] = '\0';
}

/* Called with drv->lock held. Returns true if the battery reading changed. */
static bool miracletek_handle_msg(struct miracletek_drvdata *drv,
				  const u8 *m, size_t n)
{
	int cmd = m[0] & 0x0f;
	int pct, chr = 0;
	bool chg;

	if (cmd == CMD_QUERY_RESULT) {
		if (!json_int(m + 2, n - 2, "\"battery\":", &pct))
			return false;
		json_int(m + 2, n - 2, "\"chr\":", &chr);
		chg = chr == 1;
		if (!drv->battery && !drv->model[0])
			json_str(m + 2, n - 2, "\"dn\":\"",
				 drv->model, sizeof(drv->model));
	} else if (cmd == CMD_NOTIFY && n >= 5 && m[2] == NOTIFY_BATTERY) {
		pct = m[3];
		chg = m[4] == 1;
	} else {
		return false;
	}

	pct = clamp(pct, 0, 100);
	drv->got_reply = true;
	drv->missed_polls = 0;
	if (pct == drv->capacity && chg == drv->charging)
		return false;
	drv->capacity = pct;
	drv->charging = chg;
	return true;
}

/* Called with drv->lock held. */
static bool miracletek_parse_stream(struct miracletek_drvdata *drv)
{
	static const u8 sync[4] = { 0xff, 0xff, 0xff, 0xff };
	bool changed = false;
	size_t i = 0;

	for (;;) {
		const u8 *s = drv->stream + i;
		size_t avail = drv->stream_len - i;
		const u8 *p = find_bytes(s, avail, (const char *)sync, 4);
		size_t off, n;

		if (!p) {
			/* keep up to 3 bytes that may start a sync marker */
			i = drv->stream_len > 3 ? drv->stream_len - 3 : 0;
			break;
		}
		off = p - drv->stream;
		if (off + 6 > drv->stream_len) {
			i = off;
			break;
		}
		n = ((drv->stream[off + 4] << 4) & 0xf00) | drv->stream[off + 5];
		if (n < 2) {
			i = off + 4;
			continue;
		}
		if (n + 4 > STREAM_LEN) {	/* can never fit: skip marker */
			i = off + 4;
			continue;
		}
		if (off + 4 + n > drv->stream_len) {
			i = off;
			break;
		}
		changed |= miracletek_handle_msg(drv, drv->stream + off + 4, n);
		i = off + 4 + n;
	}

	if (i) {
		memmove(drv->stream, drv->stream + i, drv->stream_len - i);
		drv->stream_len -= i;
	}
	return changed;
}

static int miracletek_raw_event(struct hid_device *hdev,
				struct hid_report *report, u8 *data, int size)
{
	struct miracletek_drvdata *drv = hid_get_drvdata(hdev);
	unsigned long flags;
	bool changed, need_register;
	const u8 *payload;
	size_t len;

	if (!drv || size < 3)
		return 0;
	if (drv->wired) {
		/* the mouse itself: plain data packets, no channel byte */
		if ((data[0] & PKT_TYPE_MASK) != PKT_EVENT_DATA)
			return 0;
		len = min_t(size_t, data[0] & PKT_SIZE_MASK, size - 1);
		payload = data + 1;
	} else {
		/* receiver: only channel-0 (mouse) data packets matter */
		if (data[0] != (PKT_EXTENDED | CH_MOUSE) ||
		    (data[1] & PKT_TYPE_MASK) != PKT_EVENT_DATA)
			return 0;
		len = min_t(size_t, data[1] & PKT_SIZE_MASK, size - 2);
		payload = data + 2;
	}

	spin_lock_irqsave(&drv->lock, flags);
	if (drv->stream_len + len > STREAM_LEN)
		drv->stream_len = 0;		/* desync: start over */
	memcpy(drv->stream + drv->stream_len, payload, len);
	drv->stream_len += len;
	changed = miracletek_parse_stream(drv);
	need_register = !drv->battery && drv->capacity >= 0 &&
			!drv->removing && !drv->register_failed;
	/* under the lock so remove() can't unregister it underneath us */
	if (changed && drv->battery)
		power_supply_changed(drv->battery);
	spin_unlock_irqrestore(&drv->lock, flags);

	if (need_register)
		schedule_work(&drv->register_work);
	return 0;	/* let hidraw see everything too */
}

/* ---- battery registration & polling ----------------------------------- */

/*
 * UPower decides a device battery's kind (mouse/keyboard/...) only from the
 * input devices that share its parent, walking up through hid and USB
 * interfaces to the whole USB device. The receiver has a keyboard interface
 * and UPower ranks keyboard above mouse, so a battery hung off the receiver
 * always shows as a keyboard. Instead we hang it off a small anchor device
 * together with a silent input device that only advertises mouse
 * capabilities. It never emits events; a udev rule sets
 * LIBINPUT_IGNORE_DEVICE on it so compositors skip it.
 */
/* Keep only [A-Za-z0-9] of a device-supplied string (used in sysfs names). */
static void sanitize_id(char *dst, size_t dstlen, const char *src)
{
	size_t n = 0;

	for (; *src && n + 1 < dstlen; src++)
		if (isalnum(*src))
			dst[n++] = *src;
	dst[n] = '\0';
}

static int miracletek_create_type_hint(struct miracletek_drvdata *drv)
{
	struct hid_device *hdev = drv->hdev;
	struct input_dev *in;
	struct device *anchor;
	int ret;

	anchor = root_device_register(drv->anchor_name);
	if (IS_ERR(anchor))
		return PTR_ERR(anchor);
	drv->anchor = anchor;

	in = input_allocate_device();
	if (!in)
		return -ENOMEM;
	/*
	 * Static strings: the input_dev can outlive this driver instance if
	 * userspace still holds its evdev node open after unregister.
	 * (UPower takes the model shown to the user from this name.)
	 */
	in->name = "RAWM wireless mouse";
	in->phys = "miracletek/battery-type-hint";
	in->id.bustype = BUS_VIRTUAL;
	in->id.vendor = hdev->vendor;
	in->id.product = hdev->product;
	in->dev.parent = anchor;
	input_set_capability(in, EV_REL, REL_X);
	input_set_capability(in, EV_REL, REL_Y);
	input_set_capability(in, EV_KEY, BTN_LEFT);
	input_set_capability(in, EV_KEY, BTN_RIGHT);

	ret = input_register_device(in);
	if (ret) {
		input_free_device(in);
		return ret;
	}
	drv->type_hint = in;
	return 0;
}

static void miracletek_destroy_battery(struct miracletek_drvdata *drv)
{
	struct power_supply *psy;
	unsigned long flags;

	spin_lock_irqsave(&drv->lock, flags);
	psy = drv->battery;
	drv->battery = NULL;
	spin_unlock_irqrestore(&drv->lock, flags);
	if (psy)
		power_supply_unregister(psy);
	if (drv->type_hint) {
		input_unregister_device(drv->type_hint);
		drv->type_hint = NULL;
	}
	if (drv->anchor) {
		root_device_unregister(drv->anchor);
		drv->anchor = NULL;
	}
}

static void miracletek_register_work(struct work_struct *work)
{
	struct miracletek_drvdata *drv =
		container_of(work, struct miracletek_drvdata, register_work);
	struct hid_device *hdev = drv->hdev;
	struct power_supply_config cfg = { .drv_data = drv };
	struct power_supply *psy;
	unsigned long flags;
	char id[32];
	int ret;

	mutex_lock(&drv->reg_mutex);
	spin_lock_irqsave(&drv->lock, flags);
	/* capacity < 0: the reading was reset while this work was queued */
	if (drv->battery || drv->anchor || drv->removing ||
	    drv->register_failed || drv->capacity < 0) {
		spin_unlock_irqrestore(&drv->lock, flags);
		mutex_unlock(&drv->reg_mutex);
		return;
	}
	if (!drv->model[0])
		strscpy(drv->model, "RAWM wireless mouse", sizeof(drv->model));
	spin_unlock_irqrestore(&drv->lock, flags);

	sanitize_id(id, sizeof(id), hdev->uniq);
	strscpy(drv->serial, id, sizeof(drv->serial));
	if (!id[0])		/* no USB serial: fall back to the HID device id */
		sanitize_id(id, sizeof(id), dev_name(&hdev->dev));
	snprintf(drv->anchor_name, sizeof(drv->anchor_name),
		 "miracletek-%s", id);
	snprintf(drv->battery_name, sizeof(drv->battery_name),
		 "miracletek-%s-battery", id);

	ret = miracletek_create_type_hint(drv);
	if (ret)
		hid_warn(hdev, "type hint failed (%d), UPower may say keyboard\n",
			 ret);

	drv->battery_desc.name = drv->battery_name;
	drv->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drv->battery_desc.properties = miracletek_props;
	drv->battery_desc.num_properties = ARRAY_SIZE(miracletek_props);
	drv->battery_desc.get_property = miracletek_get_property;

	psy = power_supply_register(drv->anchor ? drv->anchor : &hdev->dev,
				    &drv->battery_desc, &cfg);
	if (IS_ERR(psy)) {
		hid_err(hdev, "power_supply_register failed: %ld (not retrying)\n",
			PTR_ERR(psy));
		goto fail;
	}
	spin_lock_irqsave(&drv->lock, flags);
	drv->battery = psy;
	spin_unlock_irqrestore(&drv->lock, flags);
	hid_info(hdev, "battery registered: %s (%d%%%s)\n",
		 drv->model, drv->capacity, drv->wired ? ", wired" : "");
	mutex_unlock(&drv->reg_mutex);
	return;
fail:
	miracletek_destroy_battery(drv);
	spin_lock_irqsave(&drv->lock, flags);
	drv->register_failed = true;
	spin_unlock_irqrestore(&drv->lock, flags);
	mutex_unlock(&drv->reg_mutex);
}

/* Forget everything read so far. Called with drv->lock held. */
static void miracletek_reset_reading(struct miracletek_drvdata *drv)
{
	drv->register_failed = false;
	drv->stream_len = 0;
	drv->capacity = -1;
	drv->charging = false;
	drv->missed_polls = 0;
	drv->got_reply = false;
	drv->model[0] = '\0';
}

static void miracletek_poll(struct work_struct *work)
{
	struct miracletek_drvdata *drv =
		container_of(work, struct miracletek_drvdata, poll_work.work);
	unsigned long flags, delay;
	bool went_stale = false, wired_check, registered;
	int ret;

	/*
	 * A mouse is plugged in by cable somewhere. If ours didn't answer the
	 * last query, it is probably that one: its wired instance now reports
	 * the battery, so drop ours instead of showing a frozen value.
	 * Re-check and reset under reg_mutex: a register_work racing with us
	 * waits for the mutex, then sees capacity < 0 and backs off.
	 */
	spin_lock_irqsave(&drv->lock, flags);
	wired_check = drv->wired_check && !drv->got_reply &&
		      (drv->battery || drv->capacity >= 0);
	spin_unlock_irqrestore(&drv->lock, flags);
	if (wired_check) {
		mutex_lock(&drv->reg_mutex);
		spin_lock_irqsave(&drv->lock, flags);
		wired_check = drv->wired_check && !drv->got_reply;
		if (wired_check)
			miracletek_reset_reading(drv);
		spin_unlock_irqrestore(&drv->lock, flags);
		if (wired_check) {
			miracletek_destroy_battery(drv);
			hid_info(drv->hdev, "mouse not answering by radio (cable in?), battery handed over\n");
		}
		mutex_unlock(&drv->reg_mutex);
	}

	spin_lock_irqsave(&drv->lock, flags);
	if (!drv->got_reply && drv->missed_polls < STALE_POLLS) {
		drv->missed_polls++;
		went_stale = drv->missed_polls == STALE_POLLS;
	}
	drv->got_reply = false;
	if (went_stale && drv->battery)
		power_supply_changed(drv->battery);
	spin_unlock_irqrestore(&drv->lock, flags);

	ret = miracletek_send_query(drv);
	if (ret)
		hid_dbg(drv->hdev, "query failed: %d\n", ret);

	spin_lock_irqsave(&drv->lock, flags);
	registered = drv->battery != NULL;
	wired_check = drv->wired_check;
	spin_unlock_irqrestore(&drv->lock, flags);

	if (wired_check)
		delay = WIRED_CHECK_MS;
	else
		delay = registered ? POLL_MS : RETRY_MS;
	/* long wq: the output report is a blocking USB transfer */
	queue_delayed_work(system_long_wq, &drv->poll_work,
			   msecs_to_jiffies(delay));
}

/*
 * Called with mt_list_lock held when the first wired mouse appears or the
 * last one goes: tell every receiver to start or stop checking whether its
 * mouse still answers, and poll it soon.
 */
static void miracletek_set_receivers_wired_check(bool on)
{
	struct miracletek_drvdata *rx;
	unsigned long flags;

	list_for_each_entry(rx, &mt_receivers, node) {
		spin_lock_irqsave(&rx->lock, flags);
		rx->wired_check = on;
		spin_unlock_irqrestore(&rx->lock, flags);
		mod_delayed_work(system_long_wq, &rx->poll_work,
				 on ? 0 : msecs_to_jiffies(UNPLUG_POLL_MS));
	}
}

/*
 * Only the vendor-defined interface (usage page 0xFF00, USB interface 2)
 * speaks the protocol; the other interfaces share the same VID/PID.
 */
static bool miracletek_is_vendor_iface(struct hid_device *hdev)
{
	struct hid_report_enum *renum = &hdev->report_enum[HID_INPUT_REPORT];
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct hid_report *rep;
	int i;

	if (intf->cur_altsetting->desc.bInterfaceNumber != VENDOR_IFACE_NUM)
		return false;

	list_for_each_entry(rep, &renum->report_list, list)
		for (i = 0; i < rep->maxfield; i++)
			if ((rep->field[i]->application >> 16) == 0xff00)
				return true;
	return false;
}

static int miracletek_probe(struct hid_device *hdev,
			    const struct hid_device_id *id)
{
	struct miracletek_drvdata *drv;
	int ret;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	if (!miracletek_is_vendor_iface(hdev))
		return 0;		/* plain pass-through interface */

	drv = devm_kzalloc(&hdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		hid_hw_stop(hdev);
		return -ENOMEM;
	}
	drv->hdev = hdev;
	drv->wired = hdev->product == PID_WIRED_MOUSE;
	drv->capacity = -1;
	spin_lock_init(&drv->lock);
	mutex_init(&drv->reg_mutex);
	INIT_LIST_HEAD(&drv->node);
	INIT_DELAYED_WORK(&drv->poll_work, miracletek_poll);
	INIT_WORK(&drv->register_work, miracletek_register_work);
	hid_set_drvdata(hdev, drv);

	/* Keep the interrupt-IN endpoint running so replies reach raw_event. */
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid_hw_open failed: %d\n", ret);
		hid_set_drvdata(hdev, NULL);
		return 0;		/* still usable via hidraw */
	}
	drv->opened = true;

	mutex_lock(&mt_list_lock);
	if (drv->wired) {
		if (mt_wired_count++ == 0)
			miracletek_set_receivers_wired_check(true);
	} else {
		unsigned long flags;

		spin_lock_irqsave(&drv->lock, flags);
		drv->wired_check = mt_wired_count > 0;
		spin_unlock_irqrestore(&drv->lock, flags);
		list_add(&drv->node, &mt_receivers);
	}
	queue_delayed_work(system_long_wq, &drv->poll_work,
			   msecs_to_jiffies(FIRST_POLL_MS));
	mutex_unlock(&mt_list_lock);
	return 0;
}

static void miracletek_remove(struct hid_device *hdev)
{
	struct miracletek_drvdata *drv = hid_get_drvdata(hdev);

	if (drv) {
		unsigned long flags;

		if (drv->opened) {
			mutex_lock(&mt_list_lock);
			if (drv->wired) {
				if (--mt_wired_count == 0)
					miracletek_set_receivers_wired_check(false);
			} else {
				list_del(&drv->node);
			}
			mutex_unlock(&mt_list_lock);
		}

		spin_lock_irqsave(&drv->lock, flags);
		drv->removing = true;
		spin_unlock_irqrestore(&drv->lock, flags);
		cancel_delayed_work_sync(&drv->poll_work);
		if (drv->opened)
			hid_hw_close(hdev);
		cancel_work_sync(&drv->register_work);
		mutex_lock(&drv->reg_mutex);
		miracletek_destroy_battery(drv);
		mutex_unlock(&drv->reg_mutex);
	}
	hid_hw_stop(hdev);
}

static const struct hid_device_id miracletek_devices[] = {
	{ HID_USB_DEVICE(0x1915, PID_RECEIVER) },	/* RAWM HS Receiver */
	{ HID_USB_DEVICE(0x1915, PID_WIRED_MOUSE) },	/* Leviathan V4, cable */
	{ }
};
MODULE_DEVICE_TABLE(hid, miracletek_devices);

static struct hid_driver miracletek_driver = {
	.name		= DRV_NAME,
	.id_table	= miracletek_devices,
	.probe		= miracletek_probe,
	.raw_event	= miracletek_raw_event,
	.remove		= miracletek_remove,
};
module_hid_driver(miracletek_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HID battery driver for Rawmtech/RAWM wireless mice");
MODULE_AUTHOR("ProSep");

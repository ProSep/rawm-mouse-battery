# RAWM mouse battery for Linux

A small Linux kernel driver that makes the battery of **RAWM (Rawmtech)
wireless mice** show up as a normal system battery, the same way a DualSense
controller or Bluetooth headphones do. Once it's loaded, the mouse appears in
UPower, so desktop battery indicators (GNOME, KDE, Noctalia, waybar, …) and
low-battery warnings pick it up without any extra configuration.

```
$ upower -i /org/freedesktop/UPower/devices/battery_miracletek_<serial>_battery
  model:                RAWM wireless mouse
  mouse
    present:             yes
    state:               discharging
    percentage:          26%
```

RAWM doesn't ship Linux software. Its configuration tool is a WebHID page, so
the protocol here was reverse-engineered from that page's JavaScript.

## Supported hardware

| Device | USB ID | Status |
|---|---|---|
| RAWM HS Receiver (2.4 GHz dongle) with **Leviathan V4** | `1915:2346` | Tested (receiver firmware G-2.0.3, mouse G-1.2.3) |
| Leviathan V4 plugged in by cable (wired / charging) | `1915:234a` | Tested |

Other RAWM mice that use the same `1915:2346` receiver will probably work too.
The protocol is shared across RAWM's lineup, but they're untested; reports are
welcome.

**While charging by cable**, the mouse stops answering over the radio and
talks over USB instead. The driver then reads the battery from the cable
connection, so you keep seeing a live percentage while it charges. The
receiver's entry steps aside until you unplug; because the two connections
report different serial numbers, UPower lists them as separate devices, but
only one exists at a time.

The driver also fixes a Linux-only wired-mode problem: without it, the mouse
stops responding a few seconds after you plug in the cable.
[wired-mode/](wired-mode/) explains why, and has a small userspace fix for
anyone not using the driver.

## Install (DKMS)

You need `dkms` and the headers for every kernel you run (for example
`linux-headers` on Arch, `linux-headers-$(uname -r)` on Debian/Ubuntu).

```bash
git clone https://github.com/ProSep/rawm-mouse-battery
cd rawm-mouse-battery

sudo mkdir -p /usr/src/hid-miracletek-1.0
sudo cp hid-miracletek/{hid-miracletek.c,Makefile,dkms.conf} /usr/src/hid-miracletek-1.0/
sudo dkms install hid-miracletek/1.0

sudo cp hid-miracletek/99-rawm-battery.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules

sudo modprobe hid-miracletek
```

The module loads automatically whenever the receiver is plugged in, and DKMS
rebuilds it on kernel updates. When it loads, your mouse may stop responding
for about a second while the receiver switches from `hid-generic` to this
driver.

### Check that it works

```bash
cat /sys/class/power_supply/miracletek-*/capacity     # e.g. 26
upower -d | grep -A8 miracletek                       # should say "mouse"
```

The first reading arrives about a second after the module loads. The mouse has
to be awake: move it once if nothing shows up.

## Uninstall

```bash
sudo modprobe -r hid-miracletek
sudo dkms remove hid-miracletek/1.0 --all
sudo rm -r /usr/src/hid-miracletek-1.0 /etc/udev/rules.d/99-rawm-battery.rules
sudo udevadm control --reload-rules
```

## Troubleshooting

- **Mouse stopped working after loading the module:** run
  `sudo modprobe -r hid-miracletek` and the receiver falls back to the generic
  driver. If it happens at boot, add `module_blacklist=hid_miracletek` to the
  kernel command line for that boot. Please open an issue with the output of
  `journalctl -k | grep -i miracletek`.
- **No battery appears:** check `journalctl -k | grep -i miracletek`. You
  should see `battery registered: <model> (NN%)`. If you don't, make sure the
  mouse is on and paired, then move it.
- **An extra "RAWM wireless mouse" shows up in your mouse settings:** the udev rule
  `99-rawm-battery.rules` isn't installed, so libinput is using the driver's
  silent helper device. Install the rule and reload the module.
- **Secure Boot:** DKMS signs the module with its own key
  (`/var/lib/dkms/mok.pub`), which has to be enrolled with `mokutil --import`
  before the kernel will load it.

## How it works

The receiver has a vendor-defined HID interface (usage page `0xFF00`, 64-byte
reports). Every report is

```
[0xC0 | channel]?  [0x80 | size]  payload[size]
```

The optional first byte addresses a radio channel (channel 0 is the mouse);
without it, the packet is for the receiver itself. The payloads form a byte
stream of messages, each prefixed with `ff ff ff ff` followed by a 4-bit
command and a 12-bit length. Every 60 seconds the driver sends the mouse a
status query:

```
c0 8d  01 0d 03 00 00 <unix time, u64 little-endian>
```

The mouse replies with a JSON blob that includes `"battery":NN` (percent) and
`"chr":1` while charging. The driver also accepts the mouse's spontaneous
battery notification (message type `0x17`). Everything else it receives is
passed through untouched, so the vendor's web configurator keeps working.

**Why there's an extra input device:** UPower decides whether a device battery
belongs to a mouse or a keyboard by looking at the input devices on the same
USB device. The receiver also has a keyboard interface (used for the mouse's
side buttons and macros), and UPower ranks keyboard above mouse. So the driver
attaches the battery to its own small device, next to a silent input device
that only advertises mouse capabilities and never sends events. The udev rule
marks that device `LIBINPUT_IGNORE_DEVICE`, so compositors ignore it.

[NOTES.md](NOTES.md) has the full reverse-engineering notes, including dead
ends.

## Optional: user access for the vendor tool and the reader

The driver doesn't need any special permissions. The vendor's WebHID
configurator and `mouse-battery.py` do, because they talk to the receiver
directly from userspace. [`udev/70-rawm.rules`](udev/70-rawm.rules) gives the
logged-in user access to just the vendor interface:

```bash
sudo cp udev/70-rawm.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

Don't make hidraw nodes world-writable (`MODE="0666"`) instead. The vendor
channel accepts commands such as reboot, factory reset and firmware-update
mode, and the other interfaces would leak every click. The rule has to be
numbered below `73` for `TAG+="uaccess"` to take effect.

## Userspace reader (debugging)

`mouse-battery.py` speaks the same protocol from userspace through hidraw. It
needs the [`hid`](https://pypi.org/project/hid/) Python package
(`python-hid` on Arch) and the udev rule above.

```bash
./mouse-battery.py -v     # LEVIATHAN V4: 26%
./mouse-battery.py -j     # {"percent": 26, "charging": false, ...}
./mouse-battery.py -d     # also dump every decoded message
```

## Not affiliated

This project isn't affiliated with or endorsed by RAWM / Rawmtech or Miracle
Tek. Use it at your own risk.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

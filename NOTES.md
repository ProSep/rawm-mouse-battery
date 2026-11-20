# Protocol and reverse-engineering notes

These notes cover how the RAWM receiver protocol was worked out, and what was
learned along the way. The driver and `mouse-battery.py` implement everything
described here.

## Source

RAWM's configuration tool is a WebHID page (`rawmtech.com/hub.html`) backed by
an obfuscated `library.min.js` (javascript-obfuscator style: a rotated string
table plus a decoder function). The string table and its rotation can be
recovered without running the file: parse the array literal, then rotate it
until the checksum expression in the bootstrap matches. Once decoded, the
relevant functions are `device_receive_data`, `recv`, `parse_cmd`,
`send_client_data`, `read_event`, `crc_process` and `send_event_query`.

## Hardware

| | |
|---|---|
| Mouse | RAWM Leviathan V4: 2.4 GHz only, no Bluetooth |
| Receiver | "RAWM HS Receiver", USB `1915:2346` (Nordic-based, ESB radio) |
| Wired mode | the mouse itself enumerates as `1915:234a` |

The receiver has five HID interfaces:

| iface | usage page | what |
|---|---|---|
| 0 | `0x0001` | mouse |
| 1 | `0x0001` | keyboard (side buttons and macros) |
| **2** | **`0xFF00`** | **vendor channel: 64-byte input and output reports, no report IDs** |
| 3 | `0x000C` | consumer |
| 4 | `0x0001` | mouse |

## Transport

Each 64-byte report on interface 2 is:

```
[0xC0 | ch]?  [type | size]  payload[size]
```

- If the first byte has its top two bits set (`0xC0 | ch`), the packet is an
  *extended* packet for ESB channel `ch`. Channel 0 is the mouse. Without that
  byte, the packet is for the receiver itself.
- `type` is the top two bits of the next byte: `0x80` = event data,
  `0x40` = buffer-size (flow control; can be ignored when you only read).
  `size` is the low 6 bits.

The payloads of each channel form a byte stream. Messages in that stream are:

```
ff ff ff ff  [cmd | len_hi << 4]  [len_lo]  ...
```

`cmd` is the low nibble of the first byte. The length is 12 bits and counts
the two header bytes too. Sending works the same way, minus the `ff`
prefix: `[0xC0|ch] [0x80|n] message...`.

Commands that matter:

| cmd | name | notes |
|---|---|---|
| `0x01` | QUERY | `01 0d 03 00 00 <unix seconds, u64 LE>` (`03` = OS_PC) |
| `0x02` | QUERY_RESULT | JSON text, optionally NUL-terminated |
| `0x0b` | NOTIFY | `[2]` = notify type, body after it |
| `0x0e` | PING | keepalive, `0e 05 xx 00 08` |

A CRC wrapper (`03 <len> 24 <crc16> <message>`) exists, but it's only used
once the device reports `"crc":1`. A plain query works without it.

Notify types seen: `0x0c` ESB address list (JSON), `0x14` onboard config,
`0x16` battery voltage→percent table (u16 mV, 4100 down to 3200), `0x17`
battery (`body[0]` = percent, `body[1] == 1` = charging), `0x1c` RSSI.

## Reading the battery

Send QUERY to channel 0:

```
c0 8d  01 0d 03 00 00 <ts u64 LE>
```

The mouse answers within about 0.2 s with a QUERY_RESULT like:

```json
{"r":"G-1.2.3","dn":"LEVIATHAN V4","battery":35,"chr":0,"cpi":1600,
 "polling":1000,"crc":1,"st":60,"rf_ch":80, ...}
```

`battery` is the percentage the vendor tool shows. `chr` is 1 while charging.
The value moves in coarse steps.

## Lessons and dead ends

- **Reading RSSI by mistake.** An early attempt took the frame
  `88 ff ff ff ff 0b 04 1c NN` as the battery. Decoded properly, that's a
  receiver packet (`0x80 | 8`) holding a 4-byte NOTIFY of type `0x1c`, which
  is RSSI. That explains why the number jumped around.
- **No heartbeat or ESB address selection is needed** to talk to the mouse.
  Both were red herrings from reading the JS without the transport layer.
- **Kernel driver gets no replies.** Nothing reads the interrupt-IN endpoint
  of an interface that has no input device until something opens it.
  `hid_hw_open()` in probe fixes that. `hid_hw_raw_request()` times out
  (`-110`) on this interface; `hid_hw_output_report()` works.
- **UPower shows a keyboard.** UPower takes a device battery's kind only from
  input devices under the same USB device, and ranks keyboard above mouse.
  `UPOWER_BATTERY_TYPE` is only read for UPS devices. The driver therefore
  puts the battery under its own root device, next to a silent input device
  with mouse capabilities that libinput is told to ignore.
- **Don't toggle `authorized` on the receiver.** Doing that left it with no
  USB configuration (lit up but dead) until it was physically replugged.
- **Only one program can drive the vendor channel at a time** from userspace
  (the browser tool or the reader). The kernel driver doesn't conflict: it
  passes every report on to hidraw.
- **`hid-kysona`** (in-tree, for VXE/Kysona mice) doesn't apply: it matches a
  different vendor ID.
- **Wired mode** has a separate firmware quirk: the mouse stops sending input
  unless its vendor interface is held open. See [wired-mode/](wired-mode/).
- **With the cable in, the radio goes quiet.** The mouse stops answering
  queries through the receiver and speaks the same protocol on its own vendor
  interface (`1915:234a`, interface 2), without the `0xC0|ch` channel byte:
  send `8d 01 0d 03 00 00 <ts>`, and replies arrive as plain `0x80|size`
  packets. It reports `"chr":1` there and pushes NOTIFY `0x17` updates while
  charging. An early version of the driver only polled the receiver, so the
  percentage froze when the cable went in and disappeared five minutes later.

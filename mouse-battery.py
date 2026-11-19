#!/usr/bin/env python3
"""Read the RAWM Leviathan V4 (receiver 1915:2346) battery on Linux.

No browser, no root. Protocol taken from the vendor hub's library.min.js
(decoded 2026-11-20):

HID report framing (64-byte reports, report id 0, vendor iface 0xFF00):
    [0xC0 | ch]?  [type | size]  payload[size]
    - optional leading 0xC0|ch byte = "extended" packet for ESB channel ch
      (ch 0 = the mouse); without it the packet is for the receiver itself
    - type 0x80 = event data, 0x40 = buffer-size (flow control, ignore)
Payloads are concatenated into a per-channel byte stream of messages:
    ff ff ff ff  [cmd | len_hi<<4] [len_lo] ...   (len counts the message)
Sending is the same framing without the ff-prefix: [0xC0|ch][0x80|n] msg.

Asking the mouse (channel 0) for CMD_QUERY returns CMD_QUERY_RESULT with a
JSON blob that contains "battery" (percent) and "chr" (1 = charging) — the
same value the hub shows.
"""
import argparse
import json
import sys
import time

import hid

VID, PID = 0x1915, 0x2346
CH_MOUSE = 0
CMD_QUERY, CMD_QUERY_RESULT, CMD_NOTIFY = 0x01, 0x02, 0x0B
NOTIFY_BATTERY = 0x17
OS_PC = 3


def _open():
    for d in hid.enumerate(VID, PID):
        if d['usage_page'] == 0xff00:
            return hid.Device(path=d['path'])
    return None


def _msg(body):
    b = bytearray(body)
    n = len(b)
    b[0] |= (n >> 4) & 0xf0
    b[1] = n & 0xff
    return bytes(b)


def _query():
    ts = int(time.time())
    return _msg([CMD_QUERY, 0, OS_PC, 0, 0] + list(ts.to_bytes(8, 'little')))


def _send(dev, msg, ch=None):
    pkt = bytes([0x80 | len(msg)]) + msg
    if ch is not None:
        pkt = bytes([0xc0 | ch]) + pkt
    dev.write(pkt.ljust(64, b'\0'))


def _messages(buf):
    """Yield complete messages from a stream; return consumed byte count."""
    out, i = [], 0
    while True:
        j = buf.find(b'\xff\xff\xff\xff', i)
        if j < 0 or j + 6 > len(buf):
            break
        n = ((buf[j + 4] << 4) & 0xf00) | buf[j + 5]
        if n < 2 or j + 4 + n > len(buf):
            break
        out.append(bytes(buf[j + 4:j + 4 + n]))
        i = j + 4 + n
    return out, i


def read(timeout=4.0, debug=False):
    dev = _open()
    if not dev:
        raise RuntimeError("RAWM HS Receiver vendor interface not found "
                           "(dongle unplugged?)")
    stream = bytearray()
    try:
        _send(dev, _query(), ch=CH_MOUSE)
        deadline = time.time() + timeout
        resent = False
        while time.time() < deadline:
            if not resent and time.time() > deadline - timeout / 2:
                _send(dev, _query(), ch=CH_MOUSE)   # one retry
                resent = True
            r = dev.read(64, timeout=100)
            if not r:
                continue
            r = bytes(r)
            if r[0] & 0xc0 != 0xc0 or (r[0] & 0x3f) != CH_MOUSE:
                continue                     # receiver's own traffic
            r = r[1:]
            if not r or r[0] & 0xc0 != 0x80:
                continue                     # flow control
            stream += r[1:1 + (r[0] & 0x3f)]
            msgs, used = _messages(stream)
            del stream[:used]
            for m in msgs:
                cmd = m[0] & 0x0f
                if debug:
                    print("  msg:", m[:24].hex(' '), file=sys.stderr)
                if cmd == CMD_QUERY_RESULT:
                    txt = m[2:].split(b'\0', 1)[0].decode('utf-8', 'replace')
                    try:
                        info = json.loads(txt)
                    except ValueError:
                        continue
                    # device data is untrusted: validate the shape
                    if not isinstance(info, dict):
                        continue
                    pct = info.get('battery')
                    if not isinstance(pct, int) or isinstance(pct, bool):
                        continue
                    return {'percent': max(0, min(pct, 100)),
                            'charging': info.get('chr') == 1,
                            'device': str(info.get('dn', ''))[:64],
                            'firmware': str(info.get('r', ''))[:32]}
                elif cmd == CMD_NOTIFY and len(m) >= 5 and m[2] == NOTIFY_BATTERY:
                    return {'percent': m[3], 'charging': m[4] == 1,
                            'device': '', 'firmware': ''}
    finally:
        dev.close()
    return None


def main():
    ap = argparse.ArgumentParser(description="RAWM mouse battery reader")
    ap.add_argument('-j', '--json', action='store_true', help="JSON output")
    ap.add_argument('-v', '--verbose', action='store_true',
                    help="human-readable line instead of a bare number")
    ap.add_argument('-d', '--debug', action='store_true', help="show messages")
    ap.add_argument('-w', '--watch', type=float, default=0,
                    help="poll every N seconds")
    a = ap.parse_args()
    while True:
        try:
            res = read(debug=a.debug)
        except (RuntimeError, hid.HIDException) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        if not res:
            print("no reply from mouse (asleep or out of range?)",
                  file=sys.stderr)
            return 2
        if a.json:
            print(json.dumps(res))
        elif a.verbose:
            chg = " (charging)" if res['charging'] else ""
            print(f"{res['device'] or 'Mouse'}: {res['percent']}%{chg}")
        else:
            print(res['percent'])
        if not a.watch:
            break
        time.sleep(a.watch)
    return 0


if __name__ == "__main__":
    sys.exit(main())

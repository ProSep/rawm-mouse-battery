# Wired mode: mouse stops responding

**Symptom:** with the USB cable plugged into the Leviathan V4, the cursor
works for a few seconds and then freezes, although the mouse keeps charging.
Wireless mode is not affected.

**Cause:** in wired mode the mouse enumerates as its own USB device,
`1915:234a`, with three interfaces: mouse, keyboard, and a vendor interface
(usage page `0xFF00`, interface 2). Its firmware stops sending mouse input
shortly after that vendor interface is closed, and it starts again the moment
something opens it. On Windows the vendor software keeps it open; on Linux
nothing does. Read-only access is enough: no commands need to be sent.

Measured with the interface closed: 0 input events in 8 s. With it held open:
about 3,700.

## Fix

`rawkeep` is a tiny daemon that finds the wired mouse's vendor hidraw node,
opens it read-only, and holds it open. It handles unplug and replug.

1. Install the udev rule, so `rawkeep` can open the node without root:

   ```bash
   sudo cp ../udev/70-rawm.rules /etc/udev/rules.d/
   sudo udevadm control --reload-rules
   ```

   The rule gives the vendor interface to the `input` group and to the
   logged-in user. Add yourself to `input` (`sudo usermod -aG input $USER`,
   then log in again) if the mouse should also work at the login screen while
   `rawkeep` runs as a lingering user service.

2. Install the daemon as a systemd user service:

   ```bash
   install -Dm755 rawkeep ~/.local/bin/rawkeep
   install -Dm644 rawkeep.service ~/.config/systemd/user/rawkeep.service
   systemctl --user daemon-reload
   systemctl --user enable --now rawkeep.service
   ```

3. Plug the cable in and check: `journalctl --user -u rawkeep -f` should show
   `holding /dev/hidrawN open`.

A 2026 firmware update made the mouse last longer without `rawkeep` (around
10–20 s instead of about 1 s), but it still stops, so `rawkeep` is still
needed.

## Remove

```bash
systemctl --user disable --now rawkeep.service
rm ~/.config/systemd/user/rawkeep.service ~/.local/bin/rawkeep
```

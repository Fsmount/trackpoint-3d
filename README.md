# TrackPoint‑3D

Emulate a 3D mouse (6DOF) using any relative 2D pointing device plus a keyboard. It creates a virtual device via `uinput`.

### Highlights

There are 3 modes: the default is orbit, pressing shift switches to tilt, pressing ctrl switches to pan.
You can toggle capture with a hotkey (default `KEY_F8`)
Optionally install as a systemd service with `.env` config

### Requirements

Linux with `/dev/uinput` and systemd
libevdev (headers to build; runtime library to run)
Root privileges (to access `/dev/uinput` and input devices)
Spacenavd

### Build

Compile: `g++ -std=c++17 -O2 trackpoint_3d.cpp -levdev -o trackpoint-3d -pthread`

### Finding Devices

- I recommend using stable symlinks under `/dev/input/by-id/` over raw `/dev/input/event*` nodes as those may change.
- Example:
  - TrackPoint/Mouse: `/dev/input/by-id/usb-...-event-mouse`
  - Keyboard: `/dev/input/by-id/usb-...-event-kbd`

### Running

#### Direct Run

- As root:
  - `./trackpoint-3d --tp /dev/input/by-id/...-event-mouse --kbd /dev/input/by-id/...-event-kbd [--gain 60] [--hotkey 66]`
  - or enable auto-detection: `./trackpoint-3d --auto [--tp-match "TrackPoint"] [--kbd-match "ThinkPad"]`
    - Uses stable symlinks from `/dev/input/by-id` (preferred) or `/dev/input/by-path`.
    - Ordered matching (case-insensitive): user rules first (`--tp-match/--kbd-match`), then default keywords, then first capable `-event-mouse`/`-event-kbd`.
    - `--list-devices` prints candidates (name + capabilities) and exits.

#### systemd install

- As root (one‑time only):
  - `./trackpoint-3d --install --tp /dev/input/by-id/...-event-mouse --kbd /dev/input/by-id/...-event-kbd [--gain 60] [--hotkey 66] [--install-path /usr/local/bin/trackpoint-3d] [--env-dir /etc/trackpoint-3d]`
  - or with auto-detection: `./trackpoint-3d --install --auto [--tp-match "TrackPoint"] [--kbd-match "ThinkPad"]`
    - Detected stable symlinks are written into the `.env` file.

### Config Reference (.env)

- `TP_EVENT=/dev/input/path/to/mouse/event`
- `KBD_EVENT=/dev/input/path/to/kbd/event`
- `TP_MATCHES=rule1; rule2; ...` (ordered, case-insensitive)
- `KBD_MATCHES=rule1; rule2; ...` (ordered, case-insensitive)
- `ON_MISSING=fail|fallback|wait|interactive` (default `fail`)
- `WAIT_SECS=N` (only if `ON_MISSING=wait`; 0 = forever)
- `GAIN=int`
- `HOTKEY=xx` (EV_KEY code; default is `KEY_F8`)

### Troubleshooting

- No `/dev/uinput`: `sudo modprobe uinput`
- Permissions: run as root
- Unit logs: `journalctl -u trackpoint-3d -f`
- Device paths changed: update `.env` and restart the service
  - Or re-run with `--auto` to detect again.
  - Detection order: `/dev/input/by-id` then `/dev/input/by-path`; within each, rules → default keywords → first capable typed device.
  - If a preferred device is unplugged, default is to fail with a candidate list. Use `--on-missing=fallback` or `--on-missing=wait` if you prefer.

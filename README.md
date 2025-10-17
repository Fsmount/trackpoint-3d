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

#### systemd install

- As root (one‑time only):
  - `./trackpoint-3d --install --tp /dev/input/by-id/...-event-mouse --kbd /dev/input/by-id/...-event-kbd [--gain 60] [--hotkey 66] [--install-path /usr/local/bin/trackpoint-3d] [--env-dir /etc/trackpoint-3d]`

### Config Reference (.env)

- `TP_EVENT=/dev/input/path/to/mouse/event`
- `KBD_EVENT=/dev/input/path/to/kbd/event`
- `GAIN=int`
- `HOTKEY=xx` (EV_KEY code; default is `KEY_F8`)

### Troubleshooting

- No `/dev/uinput`: `sudo modprobe uinput`
- Permissions: run as root
- Unit logs: `journalctl -u trackpoint-3d -f`
- Device paths changed: update `.env` and restart the service

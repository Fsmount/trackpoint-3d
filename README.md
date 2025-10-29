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

Compile: `g++ -std=c++17 -O2 trackpoint_3d.cpp -levdev -o trackpoint-3d -pthread` or `g++ -std=c++17 -O2 trackpoint_3d.cpp $(pkg-config --cflags --libs libevdev) -o trackpoint-3d -pthread`

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
    - `--list-devices` prints candidates (name + capabilities) and exits.

**Auto Modes**

- Device selection can be explicit (`--tp <path>`, `--kbd <path>`) or automatic (`--auto`, or per-device `--tp auto` / `--kbd auto`). When using any auto form, choose one of the following predictable behaviors via `--on-missing`:
- `--on-missing=fail` (default):
  - Only selects devices that match your ordered rules (`--tp-match`, `--kbd-match`). If no rule matches, it fails (does not fall back to keywords/first device).
  - Use this when you only want your preferred external device(s) and nothing else.
- `--on-missing=fallback`:
  - Selection order per device: rules (`--*-match`) → default keywords → first capable typed device.
  - Use this when you want it to "just work" with the best available device.
- `--on-missing=interactive`:
  - Requires a TTY.
  - Uses rules first; if none match, lists candidates and lets you choose.
  - For one-off runs; not suitable for unattended services.
- `--on-missing=wait`:
  - Requires `--wait-secs N` (N>0 to bound; 0 = wait forever).
  - Uses rules; if none match, waits until a matching device appears, then proceeds. Fails on timeout.

Notes

- `--tp-match` applies only when TP is auto; `--kbd-match` applies only when KBD is auto.
- `--wait-secs` is only meaningful with `--on-missing=wait`.
- You can mix strategies per device by combining explicit path for one device and auto for the other.
- `--list-devices` is exclusive; do not combine with run or install flags.

Conflicts (these error)

- `--list-devices` cannot be combined with any other flags.
- `--wait-secs` requires `--on-missing=wait`.
- `--wait-secs` must be non-negative; `0` means wait forever.
- `--tp-match` requires TP auto selection (`--auto` or `--tp auto`).
- `--kbd-match` requires KBD auto selection (`--auto` or `--kbd auto`).
- `--install-path`, `--service-name`, and `--env-dir` require `--install`.
- `--on-missing=interactive` requires a TTY to prompt; in non-TTY contexts it fails if no rule-based match is found.
- `--on-missing=wait|interactive` requires that at least one device is auto-selected; otherwise the policy has no effect.
- `--auto` must not be combined with both `--tp <path>` and `--kbd <path>` (it would have no effect).

#### systemd install

- As root (one‑time only):
  - `./trackpoint-3d --install --<desired flags to install>`

### Examples

- Strict external TP, any keyboard by explicit path:
  - `./trackpoint-3d --tp auto --on-missing=fail --tp-match "Logitech MX" --kbd /dev/input/by-id/...-event-kbd`
- Prefer best available for both devices:
  - `./trackpoint-3d --auto --on-missing=fallback --tp-match "TrackPoint" --kbd-match "ThinkPad"`
- Prompt if preferred not found:
  - `./trackpoint-3d --auto --on-missing=interactive --tp-match "TrackPoint"`
- Wait up to 30s for preferred external TP:
  - `./trackpoint-3d --tp auto --kbd /dev/input/by-id/...-event-kbd --on-missing=wait --wait-secs 30 --tp-match "Logitech"`

### Troubleshooting

- No `/dev/uinput`: `sudo modprobe uinput`
- Permissions: run as root
- Unit logs: `journalctl -u trackpoint-3d -f`
- Device paths changed: update `.env` and restart the service
  - Or re-run with `--auto` to detect again.
  - Detection order: `/dev/input/by-id` then `/dev/input/by-path`; within each, rules → default keywords → first capable typed device.
  - If a preferred device is unplugged, default is to fail with a candidate list. Use `--on-missing=fallback` or `--on-missing=wait` if you prefer.

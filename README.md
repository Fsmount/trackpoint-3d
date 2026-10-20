# TrackPoint-3D

Emulate a 3D mouse (6DOF) using a relative pointing device and keyboard.
Requires Linux, `/dev/uinput`, and Nix with flakes enabled. The installed
service also requires systemd. For applications using the SpaceNavigator API,
install and run spacenavd.

## Controls

- F8 toggles capture; configure another key if needed.
- Orbit by default; hold Shift for tilt or Ctrl for pan.
- During capture, mouse buttons and wheel events pass through a virtual mouse.
- Capture changes wait until held mouse buttons are released.

## Try without installing

Build the binaries, select devices into a temporary configuration file, and run
in the foreground.

```sh
nix build github:Fsmount/trackpoint-3d
trial_dir=$(mktemp -d)
trial_config="$trial_dir/config"
sudo ./result/bin/trackpoint-3d-configure --config "$trial_config"
sudo ./result/libexec/trackpoint-3d --config "$trial_config"
# After Ctrl+C stops the daemon:
sudo rm -- "$trial_config"
rmdir -- "$trial_dir"
```

`/dev/uinput` must already exist. If it does not, `sudo modprobe uinput`
loads the kernel module for the current boot.

Both executables require an explicit absolute `--config` path.

## Install

Build as your regular user; use `sudo` to install and configure:

```sh
nix build github:fsmount/trackpoint-3d
sudo ln -sfnT "$(readlink -f result)" /nix/var/nix/gcroots/trackpoint-3d
sudo install -Dm644 result/share/trackpoint-3d/trackpoint-3d.service /etc/systemd/system/trackpoint-3d.service
echo uinput | sudo tee /etc/modules-load.d/trackpoint-3d.conf
sudo modprobe uinput
sudo systemctl daemon-reload
sudo ./result/bin/trackpoint-3d-configure --config /etc/trackpoint-3d/trackpoint-3d.env
sudo systemctl enable --now trackpoint-3d
```

Use `nix develop github:fsmount/trackpoint-3d` for a development shell.

## Configure

Choose devices and gain, then press the capture key on the selected keyboard.
Enter `q` at text prompts to cancel.

```sh
sudo ./result/bin/trackpoint-3d-configure --config /etc/trackpoint-3d/trackpoint-3d.env
sudo systemctl restart trackpoint-3d
```

For manual edits, use `sudoedit /etc/trackpoint-3d/trackpoint-3d.env`, then restart:

| Setting | Value |
| --- | --- |
| `TP_EVENT` | Quoted pointer path; prefer `/dev/input/by-id` or `by-path` |
| `KBD_EVENT` | Quoted keyboard path; prefer `/dev/input/by-id` or `by-path` |
| `GAIN` | Positive finite decimal; default `60` |
| `HOTKEY` | Keyboard key name or numeric keycode; default `KEY_F8` (`66`) |

Invalid known settings prevent startup, unknown settings warns and are skipped. Diagnostics identify the configuration file, line, and setting name.

Inspect status with `systemctl status trackpoint-3d` and logs with
`journalctl -u trackpoint-3d -f`.

## Upgrade

Repeat the first three installation commands, then:

```sh
sudo systemctl daemon-reload
sudo systemctl restart trackpoint-3d
```

# evdev-bridge

Bridges Sunshine's virtual input devices (uinput/evdev) to Hyprland via native Wayland protocols.

## Why?

When running Hyprland in an LXC container, there is no libinput backend — the compositor cannot read from `/dev/input/` devices. Sunshine (game streaming server) creates virtual keyboard/mouse devices via uinput, but Hyprland never sees them.

This tool reads evdev events from Sunshine's "Mouse passthrough" and "Keyboard passthrough" devices and injects them into Hyprland using:
- `zwlr_virtual_pointer_manager_v1` — for mouse movement, clicks, and scrolling
- `zwp_virtual_keyboard_v1` — for keyboard input

## Build

```bash
# Dependencies: wayland-client, libxkbcommon, wlr-protocols
sudo pacman -S wayland libxkbcommon wlr-protocols

make
```

## Usage

```bash
export WAYLAND_DISPLAY=wayland-1
export XDG_RUNTIME_DIR=/run/user/1000
./evdev-bridge
```

The tool will automatically find Sunshine's input devices and start forwarding events.

## Install

```bash
make install
```

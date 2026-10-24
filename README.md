# ProOne-PDT-6002-full-area-daemon
since i bought my optical pen PDT 6002 i had a problem in linux, this product have two areas one for android phone which is small and one for monitors that are build for desktop or bigger. however the problem was the fact that my tablet couldnt recognize large area inside of linux in other os it had a driver to fix this issue, in linux i build this

This is an early C++ user-space driver for the tablet identified in the supplied
Linux diagnostics as USB `08f2:6811` (reported by Linux as a Gotop/SZ PING-IT
T1161 device). It sends the HID feature report used by compatible Gotop-based
tablets to request full-area mode, reads their 64-byte interrupt reports, and
publishes pen and tablet-button events through `/dev/uinput`.

The USB mode command and report layout are based on community driver research
for other `08f2:6811` tablets. The vendor/product ID is reused by white-label
models, so the report layout, pressure calibration, and button mapping still
need confirmation on this exact ProOne unit. Do not install this as a system
service yet; first run it interactively and compare movement, pressure, and
buttons on the large and small areas.

Protocol references: [MX002 Linux user-space driver](https://github.com/marvinbelfort/mx002_linux_driver)
and [OpenTabletDriver device-support request for Gotop T505 `08f2:6811`](https://github.com/OpenTabletDriver/OpenTabletDriver/issues/4301).

## Build

Dependencies: C++17 compiler, CMake, pkg-config, and libusb development files.
On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
cmake -S . -B build
cmake --build build
```

## Run

The daemon needs permission to claim the tablet's USB interfaces and create a
uinput device. For an initial hardware check, run:

```sh
sudo ./build/proone-tablet-daemon
```

Stop with Ctrl+C. The program releases its USB interfaces and destroys its
virtual input device when it exits. A udev rule and systemd unit can be added
after the device has been validated.

For packet diagnostics, pass `--debug`. Debug logging is off by default to
avoid terminal output in the input loop. Input events for each USB report are
batched into one `/dev/uinput` write to reduce per-report system-call overhead.

## Known first-version limitations

- The driver is currently specific to the `08f2:6811` protocol family.
- Pressure normalization and shortcut-key defaults are provisional.
- It does not yet reconnect automatically after unplug/replug.
- Check the output of `evtest` on the created virtual device before relying on
  it in drawing applications.


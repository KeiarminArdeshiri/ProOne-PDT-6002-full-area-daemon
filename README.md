# ProOne-PDT-6002-full-area-daemon
since i bought my optical pen PDT 6002 i had a problem in linux, this product have two areas one for android phone which is small and one for monitors that are build for desktop or bigger. however the problem was the fact that my tablet couldnt recognize large area inside of linux in other os it had a driver to fix this issue, in linux i build this

This is an early C++ user-space driver for the tablet identified in the supplied
Linux diagnostics as USB `08f2:6811` (reported by Linux as a Gotop/SZ PING-IT
T1161 device). It sends the HID feature report used by compatible Gotop-based
tablets to request full-area mode, reads their 64-byte interrupt reports, and
publishes pen and tablet-button events through `/dev/uinput`.

The USB mode command and report layout are based on community driver research
for other `08f2:6811` tablets. The vendor/product ID is reused by white-label
models, so this protocol may need changes for other tablets with the same ID.
The pressure response is linear and calibrated from reports collected from
this ProOne unit.

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
virtual input device when it exits.

For packet diagnostics, pass `--debug`. Debug logging is off by default to
avoid terminal output in the input loop. Input events for each USB report are
batched into one `/dev/uinput` write to reduce per-report system-call overhead.

## Pressure settings

The recommended pressure response is a linear curve across the full virtual
pressure range. Hover reports produce zero pressure; contact maps from 0 to
5000 as the pen moves from the lightest contact to the calibrated firm-pressure
point. The raw tablet pressure is inverted, so lower raw readings mean more
pressure. Contact begins at raw pressure 1400 or below and releases at 1410 or
above to avoid brief fluctuations near the threshold lifting the pen.

Pressure curves and the approved/recommended profiles are recorded in
`pressure-settings.txt`.

## Start automatically with systemd

The included unit file runs the daemon as root, loads the `uinput` kernel
module, and restarts the process if it exits with an error. Install the
executable to `/usr/local/bin` and install the service unit:

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build --prefix /usr/local
sudo install -D -m 0644 packaging/proone-tablet-daemon.service \
  /etc/systemd/system/proone-tablet-daemon.service
sudo systemctl daemon-reload
sudo systemctl enable --now proone-tablet-daemon.service
```

Check the service and follow its logs:

```sh
systemctl status proone-tablet-daemon.service
sudo journalctl -u proone-tablet-daemon.service -f
```

After changing the source code, rebuild, reinstall the executable, and restart
the service to load the new version:

```sh
cmake --build build
sudo cmake --install build --prefix /usr/local
sudo systemctl restart proone-tablet-daemon.service
```

Stop the daemon, or disable it from starting at boot:

```sh
sudo systemctl disable --now proone-tablet-daemon.service
```

## Known first-version limitations

- The driver is currently specific to the `08f2:6811` protocol family.
- Shortcut-key defaults are provisional.
- On USB disconnect, systemd retries the process every five seconds; the
  daemon itself does not rediscover a tablet while it is already running.
- Check the output of `evtest` on the created virtual device before relying on
  it in drawing applications.

# Installation Guide

This document covers two workflows:

- End users installing from a `.deb` package.
- Developers building, modifying, and installing from source.

## End Users: Install From `.deb`

Use this path if you already have a built package such as:

```bash
fcitx5-lotus_*.deb
```

Install it with:

```bash
sudo apt install ./fcitx5-lotus_*.deb
```

After installation, restart Fcitx5:

```bash
fcitx5 -r
```

If Lotus does not appear or the old behavior is still active, log out and log back in.

Open the Fcitx5 configuration tool:

```bash
fcitx5-configtool
```

Then add or select `Lotus` as an input method.

Lotus settings can be opened with:

```bash
fcitx5-lotus-settings
```

The package should automatically install and enable the Lotus uinput server. If the server is not running, enable it manually:

```bash
sudo systemctl enable --now fcitx5-lotus-server@$USER.service
```

Check its status:

```bash
systemctl status fcitx5-lotus-server@$USER.service
```

## Developers: Build And Install From Source

Install build dependencies on Ubuntu/Debian:

```bash
sudo apt update
sudo apt install -y cmake extra-cmake-modules \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev fcitx5-modules-dev \
  libinput-dev libudev-dev g++ golang gettext hicolor-icon-theme pkg-config \
  libx11-dev python3-qtpy python3-dbus acl fcitx5
```

Clone the repository with submodules, or initialize submodules if the source tree already exists:

```bash
git submodule update --init --recursive
```

Configure and build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
```

Install the local build:

```bash
sudo cmake --install build
```

Set up the uinput server files after a direct source install:

```bash
sudo modprobe uinput
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo systemd-sysusers
sudo systemctl daemon-reload
sudo systemctl enable --now fcitx5-lotus-server@$USER.service
```

Restart Fcitx5 after every addon rebuild/install:

```bash
fcitx5 -r
```

If Fcitx5 still keeps the old shared object in memory, log out and log back in.

## Build A `.deb` Package

Install packaging tools:

```bash
sudo apt install -y debhelper devscripts fakeroot
```

Build the package into the local `output/` directory:

```bash
./scripts/build-deb-output.sh
```

The generated files will be placed in `output/`:

```bash
ls output/
```

Install the generated package:

```bash
sudo apt install ./output/fcitx5-lotus_*.deb
```

## Useful Development Commands

Check current changes:

```bash
git status --short
git diff
```

Rebuild only after source edits:

```bash
cmake --build build -j"$(nproc)"
sudo cmake --install build
fcitx5 -r
```

Run the settings GUI directly:

```bash
fcitx5-lotus-settings
```

Check whether Fcitx5 is loading the installed Lotus addon:

```bash
pgrep -a fcitx5
tr '\0' '\n' < /proc/$(pgrep -n fcitx5)/maps | grep liblotus
```

If the path shows `(deleted)`, Fcitx5 is still using an old in-memory addon. Restart Fcitx5 or log out and log back in.

## Notes For Modified Local Builds

When installing from source, always configure with:

```bash
-DCMAKE_INSTALL_PREFIX=/usr
```

Without this, helper scripts such as `fcitx5-lotus-settings` may point to `/usr/local/...` while files are installed under `/usr/...`.

For browser chat boxes and fast typing in `Uinput (Smooth)`, timing behavior can depend on the browser, compositor, and system load. Rebuild and reinstall after changing input timing code, then restart Fcitx5 before testing.

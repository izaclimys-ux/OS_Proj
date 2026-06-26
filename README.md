# KB Analytics — Keyboard Analytics Kernel Module

A Linux Loadable Kernel Module (LKM) for Raspberry Pi that:
- Detects USB device plug/unplug events
- Exposes a character device (`/dev/kb_analytics`) for user-space ↔ kernel messaging
- Hooks into the Linux input subsystem to track keyboard analytics
- Reports stats via `/proc/kb_stats`

---

## Project Structure

```
kb_analytics/
├── kb_module.c          # Kernel module (LKM)
├── userspace_app.c      # User-space C program (write/read hello-world)
├── Makefile             # Builds both .ko and userspace binary
├── run.sh               # Automation script (build → insmod → run → dmesg → stats)
├── 99-kb-analytics.rules# udev rule for USB hotplug auto-trigger
└── README.md            # This file
```

---

## Prerequisites (Raspberry Pi OS / Debian)

```bash
# Kernel headers for your running kernel
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential gcc
```

> On a stock Raspberry Pi OS the headers are at  
> `/lib/modules/$(uname -r)/build`

---

## Quick Start

```bash
# 1. Clone / copy the project
cd kb_analytics/

# 2. Run the automation script (builds, loads, demos everything)
sudo ./run.sh
```

That single command:
1. Runs `make` → builds `kb_module.ko` and `userspace_app`
2. Runs `insmod kb_module.ko` → loads the module
3. Runs `./userspace_app` → hello-world write()/read() exchange
4. Runs `dmesg | grep kb_analytics` → shows kernel log
5. Runs `cat /proc/kb_stats` → shows keyboard analytics

---

## Manual Steps (step by step)

```bash
# Build
make

# Insert module
sudo insmod kb_module.ko

# Verify it loaded
lsmod | grep kb_module
ls -l /dev/kb_analytics
cat /proc/kb_stats

# Run user-space program
sudo ./userspace_app

# Check kernel messages
dmesg | grep kb_analytics | tail -20

# Remove module
sudo rmmod kb_module
```

---

## USB Hotplug Auto-Trigger (optional)

Install the udev rule so the module loads automatically whenever a USB device is plugged in:

```bash
# Copy files to a permanent location
sudo mkdir -p /opt/kb_analytics
sudo cp kb_module.ko userspace_app run.sh /opt/kb_analytics/

# Install udev rule
sudo cp 99-kb-analytics.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules

# Now plug in any USB device — the module will auto-load
# Check the kernel log afterward:
dmesg | tail -20
```

---

## What the Kernel Module Tracks

| Metric | Where |
|--------|-------|
| Total key presses | `/proc/kb_stats` |
| Typing speed (keys/min) | `/proc/kb_stats` |
| Per-key press counts | `/proc/kb_stats` |
| Top-10 most-used keys | `/proc/kb_stats` |
| Hotkey combos (Ctrl/Alt+key) | `/proc/kb_stats` + `dmesg` |
| USB plug/unplug events | `dmesg` |

---

## Sub-commands

```bash
sudo ./run.sh              # Full setup (default)
sudo ./run.sh stats        # Show /proc/kb_stats only
sudo ./run.sh unload       # Remove the kernel module
sudo ./run.sh udev_hotplug # (called by udev automatically)
```

---

## How the write()/read() Exchange Works

```
User space                         Kernel space
──────────                         ────────────
open("/dev/kb_analytics")    ──►   dev_open()
write("Hello World from      ──►   dev_write()  → stores msg, prepares reply,
       the user space")                            logs via pr_info()
read()                       ◄──   dev_read()   → returns "Hello World from
                                                   the kernel space"
```

The kernel logs both sides via `pr_info()`, visible with `dmesg`.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `insmod: ERROR: could not insert module` | Check `dmesg` — usually a missing symbol or wrong kernel version |
| `cannot open /dev/kb_analytics` | Module not loaded, or udev hasn't created the node yet |
| `/proc/kb_stats` shows no data | Type some keys on the connected keyboard after loading |
| `make: *** No rule to make target` | Install `raspberrypi-kernel-headers` |
| `Permission denied` on `run.sh` | Run with `sudo`; or `chmod +x run.sh` |

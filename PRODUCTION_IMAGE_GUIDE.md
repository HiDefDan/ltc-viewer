# Production Image Guide (Single Source)

This is the canonical deployment process for building a production CM5 image for LTC Viewer.

## Scope

This guide replaces scattered setup flow from older docs for production rollout.

- Boot and kernel settings
- Package dependencies
- HiFiBerry LTC capture routing
- Service installation and validation
- Optional real-time kernel path
- Image-ready filesystem payload

## 1. Hardware Baseline


## 1a. Display Backend Migration: DRM Dumb Buffers → GBM/EGL

### Rationale
The initial appliance uses DRM/KMS with software rendering into dumb buffers for single-output, low-latency display. For multi-output (DSI + 2x HDMI) and future-proofing, the project is migrating to a dual-backend architecture:

- **Current:** DRM/KMS dumb buffer (CPU blit, single-threaded, proven low-latency)
- **Target:** GBM/EGL/OpenGL ES (GPU-accelerated, multi-output, async present, hardware rotation)

### Migration Plan
1. Introduce a display backend abstraction (control: DRM, experimental: GBM/EGL)
2. Wire main.c and font.c to use the backend interface
3. Add runtime/backend selection (env or config)
4. Incrementally port rendering to OpenGL ES shaders and texture atlas
5. Validate latency and cadence on all outputs before switching default

### Required Packages (Debian/RaspiOS)
```bash
sudo apt update
sudo apt install -y libgbm-dev libegl1-mesa-dev libgles2-mesa-dev mesa-utils
```

### Notes
- No X11/Wayland required; pure KMS/DRM + GBM/EGL is sufficient
- EGL backend will support triple-buffering, hardware rotation, and per-output atomic present
- The migration is staged: fallback to CPU backend is always available

### Branching Policy
- All GBM/EGL work is on `feature/gbm-egl-backend` (or sub-branches)
- `drm-kms-implementation` remains the stable, validated baseline

### Acceptance Criteria
- No regression in end-to-end latency or xrun count
- Stable multi-output present at 60Hz on all displays
- Field fallback to CPU backend always available
## 2. Install Dependencies

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y \
  git pkg-config build-essential xxd sox \
  libdrm-dev libpng-dev libmicrohttpd-dev libwebsockets-dev \
  libltc-dev libasound2-dev libdrm-tests
```

If cloud-init and old dependencies are removed in your image flow:

```bash
sudo apt autoremove --purge
```

Run once without `-y` first in release prep and review the list.

## 3. Apply Boot Files

Canonical files are stored under:

- `image-root/boot/firmware/config.txt`
- `image-root/boot/firmware/cmdline.txt`

Apply:

```bash
# IMPORTANT: image-root cmdline uses a placeholder token for root PARTUUID.
# Replace it on target before reboot.
sudo install -m 644 image-root/boot/firmware/config.txt /boot/firmware/config.txt
sudo install -m 644 image-root/boot/firmware/cmdline.txt /boot/firmware/cmdline.txt
```

Check current root PARTUUID on the target:

```bash
findmnt -no PARTUUID /
```

If needed, update the cmdline root entry before reboot:

```bash
ROOT_PARTUUID="$(findmnt -no PARTUUID /)"
sudo sed -i -E "s#root=PARTUUID=[^ ]+#root=PARTUUID=${ROOT_PARTUUID}#" /boot/firmware/cmdline.txt
```

Notes:
- cmdline keeps serial backdoor (`console=serial0,115200`)
- cmdline suppresses local console noise (`quiet loglevel=3 vt.global_cursor_default=0`)
- CPU isolation is enabled (`isolcpus=3 nohz_full=3 rcu_nocbs=3`)

### Display Refresh Policy

- DSI path is fixed at 60 Hz scanout (panel-limited).
- HDMI path can run different refresh targets where an LTC-follow workflow needs it.
- LTC decode/update cadence remains independent from fixed DSI scanout timing.


## 4. Install Application and System Files

Build and install binaries:

```bash
cd ~/ltc-viewer
make clean && make -j2
sudo make install
```

Install canonical service/config payload from `image-root`:

```bash
sudo install -m 644 image-root/etc/systemd/system/ltc-timecode.service /etc/systemd/system/ltc-timecode.service
sudo install -m 644 image-root/etc/systemd/system/ltc-config.service /etc/systemd/system/ltc-config.service
sudo install -m 440 image-root/etc/sudoers.d/ltc-config /etc/sudoers.d/ltc-config
sudo install -m 644 image-root/etc/ltc-viewer/config.json.default /etc/ltc-viewer/config.json.default
```

Enable services:

```bash
sudo systemctl daemon-reload
sudo systemctl enable ltc-config ltc-timecode
sudo systemctl restart ltc-config ltc-timecode
```

## 5. Console Cleanup for Headless Field Units

```bash
sudo systemctl mask getty@tty{1..6}.service
```

## 5a. Swap Policy for Low-Latency Appliance Units

For dedicated LTC appliances, disable generated swap/zram so boot critical path and RT jitter are not impacted by swap setup/compression work.

```bash
sudo install -d -m 755 /etc/rpi/swap.conf.d
printf "[Main]\nMechanism=none\n" | sudo tee /etc/rpi/swap.conf.d/99-disable-swap.conf >/dev/null
sudo swapoff /dev/zram0 2>/dev/null || true
```

Validate after reboot:

```bash
swapon --show
systemd-analyze critical-chain ltc-timecode.service
```

Expected: no active swap devices, and no `dev-zram0.swap` in the `critical-chain` output.

## 5b. Halt/Poweroff Behavior (Watchdog Reboot Fix)

Symptom: `sudo halt` (or a shutdown that stalls) reboots the unit instead of
stopping it.

Cause: Raspberry Pi OS enables the BCM2835 hardware watchdog via
`/usr/lib/systemd/system.conf.d/40-rpi-enable-watchdog.conf`
(`RuntimeWatchdogSec=1m`, `RebootWatchdogSec=2m`), and the bootloader
EEPROM default `POWER_OFF_ON_HALT=0` means `halt` leaves the SoC powered in
a spin loop. Nothing feeds the armed watchdog in that state, so it fires
(≤60 s) and resets the SoC — a halt becomes a reboot. This is unrelated to
the application's shutdown display blanking, which runs inside the
service's `TimeoutStopSec=5` window and is SIGKILL-bounded.

Fix — make halt actually cut power (keeps the runtime watchdog's hang
protection, which a field appliance wants):

```bash
sudo rpi-eeprom-config --edit   # add: POWER_OFF_ON_HALT=1
```

Then use `sudo poweroff` (or `sudo halt`, now equivalent) to stop units.
Validate: after `sudo poweroff`, the power LED goes out and the unit stays
down until power-cycled.

## 6. HiFiBerry LTC Input Routing

Detected card is expected to be card 2 (`sndrpihifiberry`).

Check:

```bash
arecord -l
amixer -c 2 scontrols
```

### Balanced vs Unbalanced

Balanced input is preferred for long cable runs or noisy environments.

- Balanced: better common-mode noise rejection, cleaner edge transitions for LTC decode
- Unbalanced: works for short runs and clean grounding, but is more noise-sensitive

Recommended product behavior: expose a UI selector for input mode with these options:

- `Balanced (DIFF)`
- `Unbalanced (SE)`

And map to mixer commands:

```bash
# Balanced
amixer -c 2 sset "ADC Left Input" "{VIN1P, VIN1M}[DIFF]"
amixer -c 2 sset "ADC Right Input" "{VIN2P, VIN2M}[DIFF]"

# Unbalanced
amixer -c 2 sset "ADC Left Input" "VINL1[SE]"
amixer -c 2 sset "ADC Right Input" "VINR1[SE]"
```

Set gain baseline:

```bash
amixer -c 2 sset ADC 24
```

Capture validation:

```bash
arecord -D hw:2,0 -f S32_LE -r 48000 -c 2 -d 10 /tmp/ltc-test.wav
sox /tmp/ltc-test.wav -n stat
```

## 7. Runtime Validation Checklist

```bash
# DRM
ls -la /dev/dri/
modetest -c
sudo lsof -c ltc-timecode | grep /dev/dri
cat /sys/class/drm/card0-DSI-1/status
cat /sys/class/drm/card0-DSI-1/enabled
cat /sys/class/drm/card0-DSI-1/modes
cat /sys/class/drm/card2-HDMI-A-1/status
cat /sys/class/drm/card2-HDMI-A-2/status

# Service status
sudo systemctl --no-pager --full status ltc-timecode
sudo journalctl -u ltc-timecode -n 50

# Scheduler and pinning
taskset -cp $(pgrep ltc-timecode)
chrt -p $(pgrep ltc-timecode)

```

If `card0-DSI-1` reports `connected` and `enabled` while `card2-HDMI-A-1`/`card2-HDMI-A-2` report `disconnected`, the issue is likely physical HDMI wiring, device-tree overlay routing, or EDID/connector detection rather than the GBM/EGL renderer.

Note:
- The service may be allowed on CPU 2-3, but the current hot path still tends to
  stay concentrated on CPU 3 unless the work is split more explicitly.
- That is acceptable for the current single-process design; treat it as a cue to
  revisit thread partitioning if CPU 3 remains saturated after future changes.

```bash
# Throttling
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
vcgencmd get_throttled
```

## 8. Optional RT Kernel Path

Install RT kernel:

```bash
sudo apt install -y linux-image-rpi-v8-rt linux-headers-rpi-v8-rt
sudo reboot
```

Verify:

```bash
uname -r
cat /sys/kernel/realtime
```

If RT is selected as release standard, pin explicit RT kernel image in `/boot/firmware/config.txt` (`kernel=...rpi-v8-rt`).

## 9. Production Image Workflow (Raspberry Pi Imager)

1. Flash baseline Raspberry Pi OS Lite.
2. First-boot provision script applies files from `image-root` and installs package dependencies.
3. Build/install binaries and enable services.
4. Validate DRM, ALSA, scheduler, and thermal status.
5. Capture and archive a golden image for replication.

`image-root` is designed to match target filesystem paths directly, so image customization tools can copy tree-to-tree without rewriting paths.

## End-of-Day TODO 26/05/09

- Confirm whether the RT kernel path is still desired for the production image.
- If yes, update this guide with the final kernel selection and any validation notes.

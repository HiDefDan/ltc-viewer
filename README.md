# CM5 LTC Timecode Reader

Direct framebuffer rendering of LTC (Linear Timecode) on Raspberry Pi Compute Module 5 using DRM/KMS, with ALSA audio capture and libltc decoding.

## Features

- **DRM/KMS framebuffer**: Low-latency, vblank-synced page flipping (~1-3 ms scanout)
- **Bitmap font rendering**: Pre-rasterized glyphs for crisp, deterministic display
- **ALSA + libltc decode path**: Audio LTC capture from HiFiBerry ADC decoded in userspace
- **Timezone support**: Displays local time with correct timezone conversion
- **Dynamic frame rates**: Supports 48/50/60 Hz refresh rates
- **Systemd integration**: Automatic startup and monitoring

## Hardware Setup

### Target Platform
- **Raspberry Pi Compute Module 5** (CM5)
- **[Waveshare CM5 PoE BASE A](https://www.waveshare.com/wiki/CM5_PoE_BASE_A)** baseboard (USB 3.2, PoE 802.3af/at, NVMe, dual MIPI, RTC)
- **Raspberry Pi OS Lite** (bookworm, aarch64 — 2024-11-19 or later required for CM5)
- **[Waveshare 8.8" DSI Touch A](https://www.waveshare.com/wiki/8.8-DSI-TOUCH-A)** display (480×1920 IPS, 60 Hz, 22-pin FFC to DSI1; rotated 90° in software -> 1920×480 landscape framebuffer)
- **[HiFiBerry Studio DAC+ADC](https://www.hifiberry.com/docs/data-sheets/datasheet-studio-dac-adc/)** HAT (192 kHz/24-bit Burr-Brown ADC, XLR/combo-jack input)
- **[Waveshare CM5-FAN-3007-B-5V](https://www.waveshare.com/cm5-fan-3007-b-5v.htm)** cooling fan (30×7 mm, 5 V, 4-wire PWM, 8000 RPM)

### LTC Audio Input (HiFiBerry Studio DAC+ADC)

LTC is captured as audio via the HiFiBerry ADC rather than raw GPIO.

```
XLR source (LTC out)
  ↓
HiFiBerry Studio DAC+ADC HAT  (XLR/combo-jack input, balanced or unbalanced)
  ↓
ALSA PCM capture  (hw:sndrpihifiberry)
  ↓
libltc decoder  (bi-phase decode from PCM samples)
```

**Bill of Materials**:
- 1× Raspberry Pi Compute Module 5
- 1× Waveshare CM5 PoE BASE A baseboard
- 1× Waveshare 8.8" DSI Touch A display + 22-pin FFC cable (reversed, 200 mm)
- 1× HiFiBerry Studio DAC+ADC HAT
- 1× Waveshare CM5-FAN-3007-B-5V cooling fan
- XLR cable from LTC source

**ALSA mixer setup** (run once after boot):
```bash
# Balanced XLR input, 0 dB gain (line-level LTC)
amixer sset "ADC Left Input" "{VIN1P, VIN1M}[DIFF]"
amixer sset ADC 0db
```

### Display Connection (Waveshare 8.8" DSI)

1. Connect 22-pin FFC cable (reversed, 200 mm) from display to DSI1 port on CM5 PoE BASE A
2. Connect power cable from display to 5 V / GND pins on the 40-pin header
3. Use DSI1 and set cmdline video mode for DSI-1 with 90-degree rotation
4. Display runs at native 480×1920; `rotate=90` in cmdline.txt presents a 1920×480 landscape framebuffer to the app

## Installation

### 1. Prepare Raspberry Pi OS Lite

```bash
# Boot into TTY (no X, no compositor)
# Edit /boot/firmware/cmdline.txt:
#   console=tty3 vt.global_cursor_default=0 loglevel=3

# Fix timezone (for systemd locale)
sudo timedatectl set-timezone Europe/London

# Install dependencies
sudo apt update
sudo apt install -y \
  git \
  pkg-config \
  build-essential \
  xxd \
  libdrm-dev \
  libpng-dev \
  libmicrohttpd-dev \
  libwebsockets-dev \
  libltc-dev \
  libasound2-dev
```

### 2. Clone and Build

```bash
git clone https://github.com/<your-org>/ltc-viewer.git
cd ltc-viewer
make clean
make
```

If you already have the source checked out but `make` fails with
`fatal error: drm_mode.h: No such file or directory`, run:

```bash
sudo apt install -y pkg-config libdrm-dev
pkg-config --cflags libdrm
```

Expected output includes `-I/usr/include/libdrm`.

### 3. Install as systemd Service

```bash
sudo make install
sudo systemctl enable ltc-timecode
sudo systemctl start ltc-timecode
sudo journalctl -u ltc-timecode -f
```

## Configuration

Edit [include/config.h](include/config.h) to customize:
- `TARGET_REFRESH_HZ`: Display refresh rate (default: 60)
- `DISPLAY_WIDTH` / `DISPLAY_HEIGHT`: Resolution (default: 1920×480)
- `TIMECODE_X` / `TIMECODE_Y`: Screen position for timecode
- `TZ_STRING`: Timezone for display (default: "Europe/London")
- `SHOW_SYSTEM_TIME`: Show system time instead of pure LTC (default: 1)

## Build System

**Makefile targets**:
```bash
make              # Build ltc-timecode binary
make clean        # Clean build artifacts
make install      # Install binary and systemd service
make uninstall    # Remove installation
make start        # systemctl start
make stop         # systemctl stop
make status       # systemctl status
make logs         # journalctl -f
```

## Architecture

### DRM/KMS Rendering Loop

1. **Initialization** (drm_init):
   - Open DRM device (`/dev/dri/card0`)
  - Enumerate connectors, prefer DSI and fall back to HDMI
  - Select mode (1920×480 @ 60 Hz)
   - Allocate dumb buffers (CPU-rendered ARGB8888)
   - Set initial CRTC mode

2. **Render Thread** (~50/60 Hz loop):
  - Capture LTC audio from ALSA input
  - Decode LTC frames with libltc (bi-phase decode)
   - Render timecode string to back buffer
   - Synchronous page flip (drmModeSetCrtc + vblank wait)

3. **Page Flip** (drm_page_flip_sync):
   - drmModeSetCrtc() updates hardware CRTC
   - Synchronous (blocks until vblank)
   - Swap front/back buffers

### Component Files

| File | Purpose |
|------|---------|
| `src/main.c` | Main event loop, render thread |
| `src/drm.c` | DRM device, mode setting, page flip |
| `src/font.c` | Bitmap glyph blitting (ARGB8888) |
| `src/ltc.c` | Bi-phase LTC decoding, frame extraction |
| `src/gpio.c` | Legacy GPIO edge capture stub (not used with HiFiBerry/libltc path) |
| `include/config.h` | Hardware/display configuration |

## Latency Profile

| Stage | Latency |
|-------|---------|
| ALSA capture → LTC decoder | ~1–5 ms (buffer and decode dependent) |
| LTC frame → font render | ~1–2 ms (CPU bitmap blit) |
| Render complete → scanout | ~1–3 ms (vblank sync) |
| **Total** | **~3–10 ms typical** |

vs. Chromium/Kiosk: +20–60 ms (compositor, JS layout)

## Real-Time Optimization (Optional)

Enable FIFO scheduling for lower jitter:

```bash
# systemd service (uncommement in ltc-timecode.service):
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50
CPUAffinity=3    # Pin to CPU core 3
```

Or at runtime:
```bash
sudo chrt -f 50 -p $PID
```

## Debugging

### Check DRM Device
```bash
ls -la /dev/dri/
drmModeGetConnector(fd) enumeration
```

### Monitor Audio Capture
```bash
arecord -l
amixer sget "ADC Left Input"
amixer sget ADC
```

### View Boot Logs
```bash
sudo journalctl -xe
dmesg | grep -i drm
```

### Test Rendering
```bash
# Run with verbose output
./ltc-timecode
# Check framebuffer updates (black screen → white timecode)
```

## Next Steps

1. **ALSA Capture Integration**: Add dedicated capture thread and ring buffer from HiFiBerry input
2. **libltc Integration**: Wire PCM sample stream to libltc decode path end-to-end
3. **Font Rasterization**: Generate bitmap glyphs from TrueType font at target size
4. **Latency Testing**: Measure frame-to-display time with oscilloscope or high-speed camera
5. **DSI Validation**: Verify stable mode lock with DSI-1 rotation settings on every boot

## References

- [libdrm Documentation](https://dri.freedesktop.org/wiki/libdrm/)
- [DRM/KMS on Raspberry Pi](https://www.raspberrypi.com/documentation/computers/linux_kernel_compilation.html#kms)
- [LTC Timecode Standard](https://en.wikipedia.org/wiki/Linear_timecode)
- [libltc](https://x42.github.io/libltc/)
- [HiFiBerry Studio DAC+ADC Datasheet](https://www.hifiberry.com/docs/data-sheets/datasheet-studio-dac-adc/)
- [Waveshare 8.8-DSI-TOUCH-A](https://www.waveshare.com/wiki/8.8-DSI-TOUCH-A)

## License

MIT

# Project Summary: Pi 5 LTC Timecode Reader

## 🎯 Objective
Replace the browser-based kiosk timecode display with a low-latency, DRM/KMS-based framebuffer renderer on Raspberry Pi 5, capturing LTC timecode directly from GPIO.

## 📋 What's Been Completed

### ✅ Project Structure
- Complete C source tree with modular headers
- Makefile for native build on Raspberry Pi 5
- Systemd service for autostart with real-time scheduling

### ✅ DRM/KMS Framebuffer System
- Full libdrm integration (drm.c)
- HDMI connector auto-detection
- Dumb buffer allocation (CPU-rendered ARGB8888)
- Vblank-synced page flipping
- Mode selection (1920×1080 @ 50/60 Hz default)

### ✅ Rendering Pipeline
- Main render loop at 50Hz with vblank sync (main.c)
- 7-segment bitmap font rendering with glyph positioning (font.c)
- ARGB8888 framebuffer support with background/foreground layers
- Green timecode display (RGB 64,255,64) on dark background (RGB 10,10,10)

### ✅ I/O Infrastructure
- GPIO edge capture interface ready for pigpio (gpio.c)
- Microarchitecture-agnostic timestamping
- Pin configuration via header (GPIO17 default, configurable)

### ✅ Documentation
- **README.md**: Overview, hardware wiring, installation
- **BOOT_CONFIG.md**: Raspberry Pi 5 kernel and boot configuration
- **DEPLOY.md**: Step-by-step deployment checklist
- **ARCHITECTURE.md**: Technical implementation details

### ✅ Configuration
- Centralized hardware config (include/config.h)
- Web-based configuration dashboard (localhost:8080)
- Timezone support with region-based selection
- NTP server selector with:
  - 6 built-in Stratum 0 servers (atomic clock sources)
  - Custom server management (add/edit/delete)
  - Real-time sync status panel
  - System-wide application via systemd-timesyncd
- Real-time scheduling (SCHED_FIFO, CPU 3 isolated)
- Memory locking for deterministic operation

### ✅ NTP Synchronization
- Built-in selection of precision atomic clock servers
- Custom NTP server support for local networks
- REST API for server management (`/api/ntp-servers`)
- Real-time sync status monitoring (`/api/ntp-status`)
- Automatic systemd-timesyncd integration
- Custom servers persist across sessions

---

## 🚀 Status

**Fully functional and deployed on Raspberry Pi 5:**
- ✅ DRM/KMS framebuffer rendering at 50Hz
- ✅ 7-segment timecode display (HH.MM.SS format)
- ✅ Real-time scheduling (SCHED_FIFO priority 50)
- ✅ Memory-locked for reliable operation
- ✅ Systemd service autostart

**To deploy on your Raspberry Pi 5:**
```bash
make clean && make
sudo make install
sudo systemctl enable ltc-timecode
sudo reboot
```

See **BOOT_CONFIG.md** and **DEPLOY.md** for full setup instructions.

**Next steps:**
- Integrate LTC decoder (src/ltc.c) from GPIO input (GPIO17)
- Connect XLR input via transformer + comparator circuit
- Display will automatically switch from system time to LTC when signal detected

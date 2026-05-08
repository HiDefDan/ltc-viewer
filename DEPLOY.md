# Deployment Checklist

Follow these steps to deploy the LTC timecode reader to your Raspberry Pi CM5 setup.

## Pre-Deployment: Pi Setup

- [ ] Flash Raspberry Pi OS Lite (bookworm, aarch64) to SD card
- [ ] Boot Pi, connect via SSH
- [ ] Update system: `sudo apt update && sudo apt upgrade -y`
- [ ] Install build tools and dependencies: `sudo apt install -y git pkg-config build-essential xxd libdrm-dev libpng-dev libmicrohttpd-dev libwebsockets-dev libltc-dev libasound2-dev`
- [ ] Set timezone: `sudo timedatectl set-timezone Europe/London`
- [ ] Edit `/boot/firmware/config.txt` per [BOOT_CONFIG.md](BOOT_CONFIG.md)
- [ ] Edit `/boot/firmware/cmdline.txt` per [BOOT_CONFIG.md](BOOT_CONFIG.md)
- [ ] reboot

## Hardware Setup

- [ ] Mount CM5 on Waveshare CM5 PoE BASE A
- [ ] Connect Waveshare 8.8" DSI display to DSI1 with 22-pin reversed FFC cable
- [ ] Connect display power to 5V/GND header
- [ ] Mount HiFiBerry Studio DAC+ADC and connect LTC source via XLR
- [ ] Verify ALSA device is visible: `arecord -l`
- [ ] Power on Pi

## Deploy Application

```bash
# Option A: Transfer source and build on Pi (native, recommended)
scp -r /home/admin/ltc-viewer/ pi@your-pi-ip:~/
ssh pi@your-pi-ip
cd ~/ltc-viewer
make clean && make
sudo make install

# Option B: Cross-compile and copy binary (requires aarch64-linux-gnu-gcc)
make CC=aarch64-linux-gnu-gcc
scp ltc-timecode pi@your-pi-ip:~/
ssh pi@your-pi-ip
sudo cp ~/ltc-timecode /usr/local/bin/
sudo cp systemd/ltc-timecode.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## Verify Installation

```bash
# SSH into Pi
ssh pi@your-pi-ip

# Check service status
sudo systemctl status ltc-timecode

# View live logs
sudo journalctl -u ltc-timecode -f

# Manual test (run directly)
/usr/local/bin/ltc-timecode
```

## Enable Autostart

```bash
sudo systemctl enable ltc-timecode
sudo systemctl start ltc-timecode
```

## Troubleshooting

### Service won't start
```bash
sudo journalctl -u ltc-timecode -n 20
```
Check for:
- Missing libdrm library: `ldd /usr/local/bin/ltc-timecode`
- Permission denied on /dev/dri: Run as root or add user to `video` group

### No display output
```bash
ls -la /dev/dri/
modetest -c
```
Verify DSI connector exists and KMS driver is loaded.

### LTC audio input not detected
```bash
arecord -l
amixer sget "ADC Left Input"
amixer sget ADC
```

## Configuration Customization

Edit [include/config.h](include/config.h) before build:
- Adjust `TARGET_REFRESH_HZ` (default 60 for Waveshare 8.8" DSI)
- Modify `DISPLAY_WIDTH`, `DISPLAY_HEIGHT` if display mode changes
- Modify `TIMECODE_X`, `TIMECODE_Y` for screen position
- Update `TZ_STRING` for your timezone

Then rebuild:
```bash
make clean && make
sudo make install
sudo systemctl restart ltc-timecode
```

## Performance Monitoring

Check frame timing and latency:

```bash
# Monitor framerate (should be ~50 Hz)
sudo journalctl -u ltc-timecode -f | grep "Frame"

# Check for CPU throttling
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq

# Enable real-time scheduling (optional)
# Uncomment CPU scheduling lines in systemd service and reboot
```

## Next Steps

1. **Decode LTC frames**: Complete ALSA PCM -> libltc decode path in [src/ltc.c](src/ltc.c)
2. **Font rendering**: Pre-rasterize glyphs from TrueType font and embed in [src/font.c](src/font.c)
3. **Audio capture integration**: Add robust ALSA capture buffering and underrun recovery
4. **Latency measurement**: Test with oscilloscope or high-speed camera to verify ~3–5 ms total latency
5. **Systemd optimizations**: Enable FIFO scheduling and CPU isolation for minimal jitter

## References

- [README.md](README.md) — Project overview
- [BOOT_CONFIG.md](BOOT_CONFIG.md) — Raspberry Pi OS configuration
- [CROSS_COMPILE.md](CROSS_COMPILE.md) — Cross-compilation guide
- [Makefile](Makefile) — Build system

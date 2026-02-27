# Deployment Checklist

Follow these steps to deploy the LTC timecode reader to your Raspberry Pi 5.

## Pre-Deployment: Pi Setup

- [ ] Flash Raspberry Pi OS Lite (bookworm, aarch64) to SD card
- [ ] Boot Pi, connect via SSH
- [ ] Update system: `sudo apt update && sudo apt upgrade -y`
- [ ] Install build tools: `sudo apt install -y build-essential libdrm-dev`
- [ ] Set timezone: `sudo timedatectl set-timezone Europe/London`
- [ ] Edit `/boot/firmware/config.txt` per [BOOT_CONFIG.md](BOOT_CONFIG.md)
- [ ] Edit `/boot/firmware/cmdline.txt` per [BOOT_CONFIG.md](BOOT_CONFIG.md)
- [ ] reboot

## Hardware Setup

- [ ] Wire XLR/audio input to GPIO 17 (BCM 17, Pin 11) via transformer + comparator
  - See [README.md](README.md) "Wiring recap" and [BOOT_CONFIG.md](BOOT_CONFIG.md)
- [ ] Verify GPIO is working: `gpio readall | grep GPIO17`
- [ ] Connect HDMI to display (1920×1080 resolution recommended)
- [ ] Power on Pi

## Deploy Application

```bash
# Option A: Transfer source and build on Pi (native, recommended)
scp -r /home/admin/ltc/ pi@your-pi-ip:~/
ssh pi@your-pi-ip
cd ~/ltc
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
Verify HDMI connector exists and KMS driver is loaded.

### GPIO input not detected
```bash
gpio readall
# Check pin 11 (GPIO17) state transitions when LTC signal present
```

## Configuration Customization

Edit [include/config.h](include/config.h) before build:
- Change `GPIO_LTC_PIN` if using different pin
- Adjust `TARGET_REFRESH_HZ` for 48/50/60 Hz display
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

1. **Decode LTC frames**: Implement `ltc_feed_edge()` bi-phase state machine in [src/ltc.c](src/ltc.c)
2. **Font rendering**: Pre-rasterize glyphs from TrueType font and embed in [src/font.c](src/font.c)
3. **GPIO integration**: Replace pigpio stub with actual DMA edge capture in [src/gpio.c](src/gpio.c)
4. **Latency measurement**: Test with oscilloscope or high-speed camera to verify ~3–5 ms total latency
5. **Systemd optimizations**: Enable FIFO scheduling and CPU isolation for minimal jitter

## References

- [README.md](README.md) — Project overview
- [BOOT_CONFIG.md](BOOT_CONFIG.md) — Raspberry Pi OS configuration
- [CROSS_COMPILE.md](CROSS_COMPILE.md) — Cross-compilation guide
- [Makefile](Makefile) — Build system

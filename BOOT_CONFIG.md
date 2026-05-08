# Boot Configuration for CM5 LTC Reader

## Objective
Configure Raspberry Pi Compute Module 5 to boot into a minimal "kiosk mode" with no X11/Wayland, direct framebuffer access via DRM/KMS, and vblank-synced rendering.

## Steps

### 1. Edit `/boot/firmware/config.txt`

Add or modify these settings:

```ini
dtparam=audio=on
auto_initramfs=1

# Core Driver
dtoverlay=vc4-kms-v3d

# DSI panel selection (keep only one active)
# DSI1 Use
# dtoverlay=vc4-kms-dsi-waveshare-panel-v2,8_8_inch_a
# DSI0 Use
dtoverlay=vc4-kms-dsi-waveshare-panel-v2,8_8_inch_a,dsi0

# IMPORTANT: Comment this out or delete it
# disable_fw_kms_setup=1

# Ensure DSI is checked first
display_default_lcd=1

# For RPi 4/5, explicitly set FB assignment priority
# 0 = DSI/LCD, 2 = HDMI0, 7 = HDMI1
framebuffer_priority=0

# Disable auto-detect to prevent HDMI from stealing slot 0 during hotplug
display_auto_detect=0

arm_64bit=1
disable_overscan=1
arm_boost=1

[cm5]
dtoverlay=dwc2,dr_mode=host
dtparam=rtc_bbat_vchg=3000000

[all]

# HiFiBerry Studio DAC+ADC HAT
dtoverlay=hifiberry-dacplusadcpro

# CM5 fan: PWM speed control (30x7mm, 5V)
# Temperatures in millidegrees Celsius; speed 0-255
dtparam=fan_temp0=40000,fan_temp0_hyst=2000,fan_temp0_speed=100
dtparam=fan_temp1=50000,fan_temp1_hyst=3000,fan_temp1_speed=160
dtparam=fan_temp2=60000,fan_temp2_hyst=4000,fan_temp2_speed=210
dtparam=fan_temp3=70000,fan_temp3_hyst=5000,fan_temp3_speed=255
```

### 2. Edit `/boot/firmware/cmdline.txt`

Modify the existing line to add boot verbosity settings while preserving essential parameters:
original
```
console=serial0,115200 console=tty1 root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```
new
```
video=DSI-1:480x1920M@60e,rotate=90 video=HDMI-A-1:d video=HDMI-A-2:d console=tty1 console=serial0,115200 root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```

This:
- Sets DSI panel mode with 90-degree rotation
- Disables both HDMI outputs to avoid split or mirrored desktop behavior
- Keeps serial console access and local console on tty1
- **Preserves all existing critical boot parameters** (root filesystem, fsck, locale)

### 3. Disable Services for Headless Operation

```bash
sudo systemctl mask getty@tty1.service
sudo systemctl mask getty@tty2.service
```

(NetworkManager handles all networking, including WiFi and DHCP/static IP configuration.)

### 4. Configure Timezone

```bash
sudo timedatectl set-timezone Europe/London
```

(Adjust `Europe/London` to your actual timezone. Check with `timedatectl list-timezones`.)

### 5. Configure Networking

Networking is administered via `nmtui` or `nmcli` (NetworkManager). No static IP configuration needed.

### 6. Enable LTC Timecode Service on Boot

```bash
sudo systemctl enable ltc-timecode.service
```

This ensures the timecode reader starts automatically when the Pi boots.

### 7. Reboot and Verify

```bash
sudo reboot
```

With HDMI disabled in cmdline, output should appear on the DSI panel only. Once the LTC reader starts (systemd service), you'll see the timecode display.

## Verifying DRM/KMS Setup

Install DRM testing tools:

```bash
sudo apt install -y libdrm-tests
```

After boot, SSH into the Pi:

```bash
# Check KMS driver is loaded
ls -la /dev/dri/

# Expected output:
# crw-rw----+ 1 root video 226,   0 Feb 26 12:34 card0
# crw-rw----+ 1 root video 226,   0 Feb 26 12:34 render128

# List available connectors/modes
modetest -c    # Lists modes on all connectors
```

If `/dev/dri/card0` doesn't exist, check:
```bash
dmesg | grep -i drm
dmesg | grep -i vc4
dmesg | grep -i kms
```

## Real-Time Scheduling (Optional)

For lowest latency, configure CPU isolation and real-time scheduling:

### Isolate CPU Core 3

Edit `/boot/firmware/cmdline.txt` again, appending the CPU isolation parameters:

```
console=serial0,115200 console=tty3 vt.global_cursor_default=0 loglevel=3 quiet isolcpus=3 nohz_full=3 rcu_nocbs=3 root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```

If you keep DSI/HDMI routing flags, append CPU isolation to your active line instead:

```
video=DSI-1:480x1920M@60e,rotate=90 video=HDMI-A-1:d video=HDMI-A-2:d console=tty1 console=serial0,115200 isolcpus=3 nohz_full=3 rcu_nocbs=3 root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```

This reserves CPU 3 for high-priority tasks (no kernel scheduler).

### Enable SCHED_FIFO in systemd service

Uncomment in `/etc/systemd/system/ltc-timecode.service`:

```
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=50
CPUAffinity=3
```

Reload and restart:

```bash
sudo systemctl daemon-reload
sudo systemctl restart ltc-timecode
```

## Troubleshooting

### Black screen at boot
- Check `/dev/dri/card0` exists
- Run `modetest -c` to enumerate modes
- Verify DSI cable orientation and latch are correct
- Check `dmesg` for KMS/DRM errors

### LTC timecode not updating
- Verify HiFiBerry HAT is seated correctly on the 40-pin header
- Check ALSA sees the device: `arecord -l` (should list `sndrpihifiberry` or similar)
- Confirm input routing: `amixer sset "ADC Left Input" "{VIN1P, VIN1M}[DIFF]"` and `amixer sset ADC 0db`
- Verify service is running: `systemctl status ltc-timecode`
- Check logs: `journalctl -u ltc-timecode -f`

### High latency / Tearing
- Verify vblank sync is working: check `drm_page_flip_sync()` in logs
- Ensure CPU is not throttled: `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`
- If isolated CPU is enabled, verify process is pinned: `taskset -cp $$(pgrep ltc-timecode)`

## References

- [Raspberry Pi boot configuration](https://www.raspberrypi.com/documentation/computers/configuration.html)
- [Raspberry Pi firmware parameters](https://github.com/raspberrypi/firmware/blob/master/boot/overlays/README)
- [DRM/KMS on Raspberry Pi](https://www.raspberrypi.com/forums/viewtopic.php?t=317580)

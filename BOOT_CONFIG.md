# Boot Configuration for Raspberry Pi 5 LTC Reader

## Objective
Configure Raspberry Pi 5 to boot into a minimal "kiosk mode" with no X11/Wayland, direct framebuffer access via DRM/KMS, and vblank-synced rendering.

## Steps

### 1. Edit `/boot/firmware/config.txt`

Add or modify these settings:

```ini
# Minimal boot (no splash screen)
disable_splash=1

# Disable Plymouth boot animation
avoid_init=48
avoid_init=49

# Disable FKMS (use standard KMS driver)
# (Remove or comment out any `dtoverlay=vc4-fkms-v3d` lines)
```

### 2. Edit `/boot/firmware/cmdline.txt`

Modify the existing line to add boot verbosity settings while preserving essential parameters:
original
```
console=serial0,115200 console=tty1 root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```
new
```
console=serial0,115200 console=tty3 vt.global_cursor_default=0 loglevel=3 quiet root=PARTUUID=68f99be8-02 rootfstype=ext4 fsck.repair=yes rootwait cfg80211.ieee80211_regdom=GB
```

This:
- Routes console output to TTY3 (in addition to serial, leaving TTY1 for LTC app)
- Hides cursor
- Reduces boot verbosity
- Suppresses most kernel messages
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

You should see a **black screen** on HDMI. Once the LTC reader starts (systemd service), you'll see the timecode display.

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
- Verify HDMI cable is plugged in firmly
- Check `dmesg` for KMS/DRM errors

### LTC timecode not updating
- Check GPIO pin is wired and has signal
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

# CM5 LTC Timecode Viewer

Low-latency LTC timecode display appliance for Raspberry Pi CM5 using DRM/KMS framebuffer rendering and HiFiBerry audio capture.

## Quick Start (Production)

1. Download the latest image or release payload from GitHub Releases.
2. Flash to SD/NVMe with Raspberry Pi Imager.
3. Boot with the DSI panel and HiFiBerry hardware attached.
4. Verify services:
   - `sudo systemctl status ltc-timecode`
   - `sudo systemctl status ltc-config`
5. Open the configuration UI on port 8080.

## Display Behavior

- DSI output is fixed at 60 Hz, is always the primary/permanent display, and is **permanently locked to center** — there is no user-adjustable position for it.
- The renderer drives DSI **and** any connected HDMI output simultaneously. On Pi5/CM5-class hardware, DSI (RP1's `drm-rp1-dsi`) and HDMI (the main SoC's `vc4-drm`) are separate DRM devices with no shared GPU context — the backend tracks one GBM device per physical `/dev/dri/cardN` in use and gives each output its own font atlas/shader. HDMI is detected live: plugging in a cable activates it within about a second (no service restart needed), and unplugging cleanly tears it back down without affecting the DSI panel.
- Each output renders the same LTC/ToD content natively in its own connector's preferred mode and orientation — the DSI panel keeps its portrait per-glyph UV rotation, while HDMI lays the timecode out full-size in its own landscape resolution (not a letterboxed copy of the DSI canvas). HDMI always uses its EDID-preferred mode; the configured `display_width`/`display_height`/`refresh_hz` apply to DSI only.
- "Centered" is the bounding-box center of the 8-digit `HH.MM.SS.FF` string (the 2nd `.` between MM and SS marks that same midpoint — periods have zero glyph width, so the period itself is drawn one digit-width right of the true center). The background "88.88.88.88." now starts at the same x as the lit text (previously offset one digit-width left, which looked misaligned); its only decorative extra is a trailing 4th period, matching a real 7-segment display's unlit last decimal point.
- Each connected HDMI display can be repositioned independently via the web UI, keyed by its stable port name (e.g. `HDMI-A-1` vs `HDMI-A-2`) so a saved offset survives unplug/replug and reboot. The Display Settings page shows one position control per currently-attached HDMI port and updates live (over the existing config WebSocket) as displays are connected/disconnected — no page reload needed.
- Portrait DSI panels use per-glyph UV rotation in the GBM/EGL backend so text renders upright instead of stacked.
- Glyphs are rasterized at startup directly from the vendored DSEG7 Classic Bold TTF (`data/fonts/`, installed to `/usr/share/ltc-timecode/fonts/`) at each output's native pixel size — no pre-rendered image assets, no runtime resampling. The historical PNG assets were DSEG7 Classic Bold *Italic*; swapping the TTF file (same filename) changes the face for every output.
- Presentation is async page-flips: one `drmModeSetCrtc` at output activation, then `drmModePageFlip` per frame with the render loop paced by flip-completion events (a queued frame latches at the next vblank while the following frame is already being prepared).
- GBM/EGL LTC/ToD fades preserve zero-alpha endpoints correctly, eliminating the two full-brightness flashes that could appear at transition start/end.
- LTC decode and update cadence is independent from panel scanout timing.
- Diagnostic tip: verify the active DRM outputs with `sudo lsof -c ltc-timecode | grep /dev/dri` and compare against `/sys/class/drm/card1-DSI-1/status` and `/sys/class/drm/card2-HDMI-A-*/status` (card numbers vary by board) to confirm which connectors the renderer is using; `[GBM] Output N activated/deactivated` lines in the journal log hotplug transitions.

## Hardware Baseline

- Raspberry Pi CM5
- Waveshare CM5 PoE BASE A
- Waveshare 8.8 inch DSI panel
- HiFiBerry Studio DAC+ADC

## Services

- `ltc-timecode`: DRM/KMS render + LTC pipeline
- `ltc-config`: configuration daemon and web UI

## Configuration UI

- Default URL: `http://<device-ip>:8080`
- Supports timezone, network, display position, color settings, and LTC/ToD transition fade duration

## UX Framegrabs (Temporary)

- For offline UX review, the renderer supports one-shot framegrabs from the actual render buffer when `LTC_FRAMEGRAB_DIR` is set in the service environment.
- Generated files: `ltc-live.bmp` and `tod-fallback.bmp` (captured after transition fade completes).
- Clear the environment override after captures to return to normal operation.

## Debug Fade Test

- Set `LTC_FADE_TEST_PERIOD_MS` to a non-zero value to toggle between synthetic LTC-style timecode and ToD fallback without a live LTC cable.
- Example: `LTC_FADE_TEST_PERIOD_MS=1000` flips source every second and exercises the same LTC/ToD fade path used in normal runtime.
- This hook is intended for visual fade regression testing and should be unset for production operation.

## Boot Slimming Profile

- Current appliance profile keeps `avahi-daemon` (mDNS), `cron`, `systemd-timesyncd` (NTP), `NetworkManager`, `wpa_supplicant`, and `ssh` enabled.
- The following services were disabled to reduce boot-to-display latency:
   - `bluetooth.service`
   - `console-setup.service`
   - `keyboard-setup.service`
   - `udisks2.service`
   - `e2scrub_reap.service`
   - `rpi-eeprom-update.service`
- `NetworkManager-wait-online.service` was restored to keep remote Wi-Fi bring-up predictable during cold boot.
- One-line rollback:
   - `sudo systemctl enable --now bluetooth.service console-setup.service keyboard-setup.service udisks2.service e2scrub_reap.service rpi-eeprom-update.service`

## Performance Progression (May 2026)

The following measurements were captured on-device on CM5 + Waveshare 8.8 inch DSI (480x1920 @ ~60 Hz), with live LTC near 29.97 fps.

| Step | Change | CPU (single process) | avg_render |
|---|---|---:|---:|
| 0 | Baseline software-rotate-era path | ~71% | ~35 ms |
| 1 | Direct portrait render + static background caching + redraw suppression | ~47% | ~16-17 ms |
| 2 | Cadence-aware render limiting + adaptive pacing | ~30% | ~16-18 ms |
| 3 | Cached scaled glyph atlas (fast blit path) | ~18% | ~6.8-7.7 ms |
| 4 | Smoothed cached alpha path + subtle gamma tuning | ~14-15% | ~5.4-5.8 ms |
| 5 | GBM/EGL direct glyph render path | ~14-15% | ~0.10 ms |

Notes:
- `avg_render` is from the `[MAIN] Frame ... avg_render=...us` journal metric.
- Current GBM/EGL direct render path is measuring about `103 us` average per render pass on the live DSI portrait build.
- Default transition fade duration is now `500 ms`, and the GBM/EGL path keeps the GPU render loop active across refreshes for smoother crossfades.
- CPU is sampled with `ps -p <pid> -o %cpu` and reflects one process on one core.
- Small run-to-run variation is expected due to LTC cadence and system load.

Representative commands:
- `sudo journalctl -u ltc-timecode --since '90 seconds ago' --no-pager | grep '\[MAIN\] Frame' | tail -n 10`
- `ps -p <pid> -o pid,psr,pcpu,pmem,etime,cmd`

### Achievement Summary

- End-to-end display latency improved from roughly `~51 ms` to `~9.4-10.0 ms`.
- Frame-equivalent latency improved from about `~1.2-1.5 frames` to about `~0.23-0.30 frames` (rate dependent).
- Practical headline: worst measured latency is around `~0.30 frame`, well below half-frame.

## Next Tuning Pass

CPU affinity was adjusted during optimization and should be revisited as a dedicated pass. Current service policy can be reviewed in [systemd/ltc-timecode.service](systemd/ltc-timecode.service) (and image payload mirror under [image-root/etc/systemd/system/ltc-timecode.service](image-root/etc/systemd/system/ltc-timecode.service)).

## Internal Provisioning and Manufacturing

For image-root payload details, boot files, and production provisioning workflow, see [PRODUCTION_IMAGE_GUIDE.md](PRODUCTION_IMAGE_GUIDE.md).

## Change History

See [CHANGELOG.md](CHANGELOG.md) for release and optimization history.

## License

MIT

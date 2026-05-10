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

- DSI output is fixed at 60 Hz.
- HDMI output may be configured separately for dynamic-refresh workflows.
- LTC decode and update cadence is independent from panel scanout timing.

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

## Performance Progression (May 2026)

The following measurements were captured on-device on CM5 + Waveshare 8.8 inch DSI (480x1920 @ ~60 Hz), with live LTC near 29.97 fps.

| Step | Change | CPU (single process) | avg_render |
|---|---|---:|---:|
| 0 | Baseline software-rotate-era path | ~71% | ~35 ms |
| 1 | Direct portrait render + static background caching + redraw suppression | ~47% | ~16-17 ms |
| 2 | Cadence-aware render limiting + adaptive pacing | ~30% | ~16-18 ms |
| 3 | Cached scaled glyph atlas (fast blit path) | ~18% | ~6.8-7.7 ms |
| 4 | Smoothed cached alpha path + subtle gamma tuning | ~14-15% | ~5.4-5.8 ms |

Notes:
- `avg_render` is from the `[MAIN] Frame ... avg_render=...us` journal metric.
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

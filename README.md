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
- Supports timezone, network, display position, and color settings

## Internal Provisioning and Manufacturing

For image-root payload details, boot files, and production provisioning workflow, see [PRODUCTION_IMAGE_GUIDE.md](PRODUCTION_IMAGE_GUIDE.md).

## License

MIT

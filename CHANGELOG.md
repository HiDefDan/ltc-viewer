# Changelog

All notable changes to this project are documented in this file.

## Project Timeline (From Scratch)

- 2026-02-27: Initial renderer foundation (DRM/KMS framebuffer + real-time scheduling).
- 2026-02-28: Rapid feature build-out (config daemon, web sync, dynamic display modes, bounds handling, UHD support, diagnostics).
- 2026-05-08: CM5 hardware alignment and build/deployment hardening.
- 2026-05-09: DSI production pipeline finalization, LTC ingest integration, smoothing/latency instrumentation, packaging/docs completion.
- 2026-05-10: Render-path performance optimization campaign (portrait direct render, cache layers, pacing, glyph atlas, smoothing tuning, affinity updates).

## 2026-05-10

### Added
- Introduced this changelog to track runtime and rendering changes.

### Changed
- Switched portrait display path to direct portrait rendering (removed full-frame software rotation pass).
- Added static background layer caching to avoid rebuilding unlit segments every render.
- Added adaptive pacing and cadence-aware render limiting to reduce busy-loop CPU usage.
- Added glyph atlas cache with pre-scaled alpha maps for faster repeated text rendering.
- Added subtle alpha gamma tuning (`FONT_ALPHA_GAMMA=0.90`) to smooth glyph edges while retaining low render cost.
- Added optional dirty-region restore path in portrait mode to reduce framebuffer copy bandwidth.
- Added explicit main-thread CPU affinity pinning to core 3 (`sched_setaffinity`) early in startup.
- Gated render/flip work in `main` loop to fresh LTC data (`shared->fresh` path via `got_ltc`).

### Performance Snapshot (CM5 + Waveshare 8.8 DSI)
- Baseline (software-rotate era): ~71% CPU, ~35 ms avg_render.
- After direct portrait + caching: ~47% CPU, ~16-17 ms avg_render.
- After cadence limiting/pacing: ~30% CPU.
- After glyph cache + smoothed alpha path: ~14-18% CPU, ~5.4-7.7 ms avg_render.

### Notes
- Metrics are based on service journal `avg_render` and `ps` process CPU snapshots; small run-to-run variation is expected.
- Achievement headline: end-to-end latency reduced from about `~51ms` (~`1.2-1.5` frames) to about `~9.4-10.0ms` (~`0.23-0.30` frames).
- Latency telemetry now reports three microsecond fields in `[MAIN] Frame` logs:
	- `adc_dec_lat_us`: callback ingest timestamp -> decoded LTC frame timestamp.
	- `dec_disp_lat_us`: decoded LTC frame timestamp -> rendered display completion timestamp.
	- `adc_disp_lat_us`: callback ingest timestamp -> rendered display completion timestamp.
- 2026-05-10 live check (service PID 17548, 5 fresh samples):
	- `adc_dec_lat_us=2-4us`, `dec_disp_lat_us=9401-10015us`, `adc_disp_lat_us=9405-10019us`.
- CPU affinity repeatability matrix (2026-05-10, 3 fresh frame samples/profile, LTC ~25 fps):

| Profile | Service CPUAffinity | Main thread pin | avg_rendered | avg_render (us) | avg_disp_lat (ms) | ps CPU snapshot |
|---|---|---|---:|---:|---:|---:|
| A | `2 3` | CPU 3 | 33.0 | 5777.3 | 8.7 | 8.0% |
| B | `3` | CPU 3 | 33.0 | 5692.7 | 8.7 | 8.2% |

- Result: kept `CPUAffinity=3` as the standard service policy (simpler isolation, equal/better render timing in sampled runs).

## 2026-05-09

### Added
- ALSA/RtAudio LTC ingest path and libltc decoder integration.
- LTC lock/loss handling, smoothing behavior, and decode-to-display latency measurement.
- Hundredths/frame display format updates (`HH.MM.SS.FF`) and render timing metrics.

### Changed
- Renamed project LTC header to avoid system header shadowing (`ltc.h` -> `ltc-timecode.h`).
- Finalized DSI pipeline behavior and production release packaging workflow.
- Tightened Copilot/project instructions and aligned repo automation/docs for release.

## 2026-05-08

### Changed
- Updated CM5 hardware and deployment documentation.
- Hardened dependency/build guidance and aligned boot/deployment docs.

## 2026-02-28

### Added
- Configuration daemon + embedded web UI build flow.
- NTP selector support (including stratum 0/custom options).
- Dynamic display mode detection and dynamic refresh switching from config.
- WebSocket config synchronization and network monitoring path.
- Expanded DRM mode/debug diagnostics for mode selection and fallback analysis.

### Changed
- Improved positioning/bounds clamping and live UI positioning behavior.
- Added/validated UHD 50Hz support and 1080p mode reliability improvements.
- Updated Makefile/build structure for multi-binary output and generated web assets.

### Maintenance
- Stopped tracking generated `web-embedded.c` and updated ignore behavior.

## 2026-02-27

### Added
- Initial DRM/KMS framebuffer timecode renderer.
- Real-time scheduling baseline for deterministic display loop behavior.

# Changelog

All notable changes to this project are documented in this file.

## 2026-07-19 (2)

### Added
- Per-HDMI-port repositioning: each connected HDMI display can now be positioned independently, keyed by its stable port name (`drm_connector_name()`, e.g. `HDMI-A-1`) via new `hdmi_positions[]` entries in `ltc_config_t` (`include/config-management.h`, `src/config-management.c`). DSI is permanently locked to center and is no longer user-adjustable at all — the old shared `timecode_x_offset`/`timecode_y_offset` fields are removed.
- `ltc-config-daemon` gained `/api/hdmi-ports` (lists every currently-connected HDMI port + its mode across every DRM device, not just the first) and pushes `{"type":"hdmi_ports",...}` over the existing config WebSocket whenever the connected set changes, so the web UI's per-port position sections update live with no page reload.
- The web UI's Display Settings section now renders one position control block per currently-attached HDMI port dynamically, seeded from saved config and updated live via WebSocket.

### Fixed
- Corrected the background layer's horizontal position in `build_output_render_state()` (`src/main.c`): the "88.88.88.88." background now starts at the same x as the lit `HH.MM.SS.FF` text (previously offset one full digit-width to the left), so lit and unlit digits line up 1:1. The background's only remaining decorative difference from the lit text is its trailing 4th period — a real 7-segment display's unlit final decimal point — which has zero glyph width and doesn't affect layout.
- Horizontal centering is plain bounding-box centering of the 8-digit string (unchanged from before this file's HDMI work started) — an intermediate attempt anchored on the literal draw position of the 2nd `.` instead of the 8-digit block's true midpoint, which shifted the whole string a full digit-width right of center; reverted once the background-alignment fix above made the underlying asymmetry-driving concern moot.
- Fixed the underlying multi-output implementation from earlier today for real Pi5/CM5 hardware topology: DSI and HDMI are driven by **separate DRM devices** (`drm-rp1-dsi` on the RP1 southbridge vs `vc4-drm` on the main SoC), not one shared device as originally assumed. `src/display-backend-gbm.c` now opens one `ltc_gbm_device_t` (GBM device + EGL display) per physical `/dev/dri/cardN` actually in use, and each output carries its own font atlas texture/shader — copied from a same-device sibling output when one exists, built fresh otherwise. HDMI connector discovery now scans every KMS-capable card instead of stopping at the first one that opens successfully.

## 2026-07-19 (1)

### Added
- Simultaneous DSI + HDMI rendering: the GBM/EGL backend now drives the DSI panel and any connected HDMI output(s) at the same time, instead of only ever scanning out to one connector (DSI preferred, HDMI as a DSI-absent fallback).
- Live HDMI hotplug: `gbm_backend_poll_hotplug()` / `display_backend_poll_hotplug()` rescans known HDMI connectors roughly once a second from the main render loop and activates/deactivates outputs on connect/disconnect, with no service restart required.
- Each output renders the shared LTC/ToD content laid out natively for its own connector mode (`src/main.c`'s `build_output_render_state()`) — DSI keeps its configured portrait mode, HDMI always self-selects its EDID-preferred landscape mode.
- A failing secondary HDMI output (e.g. an unplug race) is deactivated and skipped without affecting DSI rendering; only a primary-output failure is treated as fatal, matching prior single-display behavior.

### Changed
- `find_display_connector()` (single-connector selection) replaced by connector enumeration + `find_crtc_for_connector()` with a CRTC exclusion list, so a second connector never gets assigned a CRTC already claimed by another active output. (Superseded later the same day — see above — once real hardware showed DSI/HDMI are on different DRM devices entirely.)

## 2026-07-18

### Added
- Added `LTC_FADE_TEST_PERIOD_MS` to toggle deterministically between synthetic LTC-style timecode and ToD fallback for fade regression testing without live LTC input.

### Fixed
- Corrected GBM/EGL fade endpoint alpha handling in `src/display-backend-gbm.c` so zero-alpha crossfade endpoints remain transparent instead of flashing full-brightness.
- Resolved the visible double-pulse on the outgoing timecode string during LTC/ToD transitions on the GPU render path.

## Project Timeline (From Scratch)

- 2026-02-27: Initial renderer foundation (DRM/KMS framebuffer + real-time scheduling).
- 2026-02-28: Rapid feature build-out (config daemon, web sync, dynamic display modes, bounds handling, UHD support, diagnostics).
- 2026-05-08: CM5 hardware alignment and build/deployment hardening.
- 2026-05-09: DSI production pipeline finalization, LTC ingest integration, smoothing/latency instrumentation, packaging/docs completion.
- 2026-05-10: Render-path performance optimization campaign (portrait direct render, cache layers, pacing, glyph atlas, smoothing tuning, affinity updates).

## 2026-05-13

### Fixed
- Corrected portrait glyph orientation in the GBM/EGL backend by applying per-glyph UV rotation for portrait DSI panels in `src/display-backend-gbm.c`.
- Confirmed the Waveshare 8.8" portrait canvas now renders upright timecode text without stacking artifacts.
- Increased default transition fade duration to `500 ms` and enabled GBM/EGL full-refresh rendering for smoother LTC/ToD crossfades.
- Validated GBM/EGL render latency at about `0.10 ms` avg_render (`avg_render=103us` over 200 recent journal samples).

## 2026-05-12

### Changed
- Added configurable RtAudio callback scheduling priority via `LTC_RTA_RT_PRIORITY`.
- Set appliance service default to `LTC_RTA_RT_PRIORITY=85` in runtime and image payload units.
- Applied HiFiBerry Pro overlay on target boot config for master-clock operation (`dtoverlay=hifiberry-dacplusadcpro`).
- Disabled generated rpi swap/zram path on target validation unit (`/etc/rpi/swap.conf.d/99-disable-swap.conf` with `Mechanism=none`).

### Validation Snapshot (CM5 4 GB + HiFiBerry DAC+ADC Pro + Waveshare 8.8 DSI)
- Boot timing after zram disable:
	- `Startup finished in 5.136s` (was `5.268s` in immediate pre-change sample)
	- `ltc-timecode.service @1.921s` (was `@2.052s`)
	- zram no longer present in `critical-chain`
- Live LTC sample (25 fps, 8 consecutive journal windows):
	- `in_out_ms=3.780-4.044ms`, avg `3.935ms`
	- `tc_end_disp_ms=3.969-4.222ms`, avg `4.108ms`
	- `tc_end_glass_mid_ms=12.285-12.538ms`, avg `12.424ms`
	- xruns stayed `0/0`
- Field camera observation: roughly `~1 frame` visible source-to-panel delay from Rosendahl MIF LTC to DSI output.

### Roadmap TODO (Post-Baseline)
- Implement PTP-disciplined timestamp domain for cross-device correlation while keeping monotonic timing for in-process latency.
- Add optional bounded AGC-lite stage (leaky peak follower + clamp + bypass) ahead of libltc decode.
- Add fault-injection test profile (GM loss, network flap, clock step) with recovery criteria and pass/fail capture.
- Add optional PPS health monitor to track LTC second-boundary phase against hardware-accurate PPS.

## 2026-05-10

### Added
- Introduced this changelog to track runtime and rendering changes.
- Added configurable LTC/ToD transition fade duration (`transition_fade_ms`) with web UI control.
- Added temporary render-buffer framegrab hook for UX review captures (`LTC_FRAMEGRAB_DIR` environment override).

### Changed
- Switched portrait display path to direct portrait rendering (removed full-frame software rotation pass).
- Added static background layer caching to avoid rebuilding unlit segments every render.
- Added adaptive pacing and cadence-aware render limiting to reduce busy-loop CPU usage.
- Added glyph atlas cache with pre-scaled alpha maps for faster repeated text rendering.
- Added subtle alpha gamma tuning (`FONT_ALPHA_GAMMA=0.90`) to smooth glyph edges while retaining low render cost.
- Added optional dirty-region restore path in portrait mode to reduce framebuffer copy bandwidth.
- Added explicit main-thread CPU affinity pinning to core 3 (`sched_setaffinity`) early in startup.
- Gated render/flip work in `main` loop to fresh LTC data (`shared->fresh` path via `got_ltc`).
- Added config model/default persistence for `transition_fade_ms` in runtime and image payload defaults.
- Updated source-state fade capture behavior to only framegrab after fade completion, avoiding transition-frame mislabels.
- Trimmed systemd startup ordering to reduce boot-to-display latency:
	- removed `Wants=network-online.target` and `After=multi-user.target` from `ltc-timecode.service`
	- removed `After=network.target` and `Before=ltc-timecode.service` from `ltc-config.service`
- Fixed intermittent dim glyph rendering after LTC/ToD transitions by including effective alpha in glyph-cache key matching.
- Added RtAudio overflow/underflow telemetry and direct millisecond latency reporting (`in_out_ms`, `tc_end_disp_ms`, `tc_start_disp_ms`, `tc_end_glass_mid_ms`).
- Added estimated LTC frame start/end sample timing metadata from libltc for improved analog-to-display latency analysis.
- Added runtime-configurable low-latency tuning controls for capture period, RtAudio queue depth, smoothing, phase advance, and live-loop sleep policy.
- Set low-latency runtime defaults for appliance operation:
	- `LTC_RTA_PERIOD_FRAMES=32`
	- `LTC_RTA_NUM_BUFFERS=2`
	- `LTC_SMOOTH_ENABLE=0`
	- `LTC_PHASE_ADVANCE_FRAMES=1`
	- `LTC_LIVE_NEED_RENDER_DIV=20`
	- `LTC_LIVE_IDLE_DIV=16`
	- `LTC_LIVE_SLEEP_MIN_US=100`
	- `LTC_LIVE_SLEEP_MAX_US=2000`
- Added persistent boot tuning service (`ltc-boot-tune.service`) to re-apply CPU governor `performance` and IRQ affinity on every boot.
- Applied additional system-level latency trimming on target validation system:
	- disabled `wpa_supplicant.service`
	- disabled `NetworkManager-wait-online.service`
	- disabled Wi-Fi radio
	- pinned IRQs `142`, `149`, `187` to CPU mask `4`
- Applied appliance boot-service slimming on target image/runtime (kept mDNS, cron, NTP):
	- disabled `bluetooth.service`
	- disabled `console-setup.service`
	- disabled `keyboard-setup.service`
	- disabled `udisks2.service`
	- disabled `e2scrub_reap.service`
	- disabled `rpi-eeprom-update.service`
	- initially disabled `NetworkManager-wait-online.service`, then restored it after field validation to avoid Wi-Fi/remote-access startup surprises

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
- 2026-05-11 long-window tuned sample (25 fps, 52 samples, 180 s capture):
	- `in_out_ms=8.890-10.097ms`, avg `9.557ms`
	- `tc_end_disp_ms=9.279-10.611ms`, avg `9.927ms`
	- `tc_start_disp_ms=49.257-50.589ms`, avg `49.907ms`
	- `tc_end_glass_mid_ms=17.595-18.927ms`, avg `18.243ms`
- 2026-05-11 persistent low-latency profile with phase advance + boot tuning (recent 15 min journal aggregate):
	- before extra trim: `in_out_ms avg=7.103ms`, `tc_start_disp_ms avg=47.456ms`
	- after extra trim: `in_out_ms avg=6.641ms`, `tc_start_disp_ms avg=46.991ms`
	- xruns remained `0/0`
- 2026-05-11 live appliance defaults confirmed after deployment:
	- `req_buf=32`, `actual_buf=32`, `num_buffers=2`, `smooth=off`, `phase_adv=1`
	- CPU governor: `performance` on all four cores
	- IRQ masks: `142=4`, `149=4`, `187=4`
- CPU affinity repeatability matrix (2026-05-10, 3 fresh frame samples/profile, LTC ~25 fps):

| Profile | Service CPUAffinity | Main thread pin | avg_rendered | avg_render (us) | avg_disp_lat (ms) | ps CPU snapshot |
|---|---|---|---:|---:|---:|---:|
| A | `2 3` | CPU 3 | 33.0 | 5777.3 | 8.7 | 8.0% |
| B | `3` | CPU 3 | 33.0 | 5692.7 | 8.7 | 8.2% |

- Result: kept `CPUAffinity=3` as the standard service policy (simpler isolation, equal/better render timing in sampled runs).
- `LTC_FRAMEGRAB_DIR=/path` (via service manager environment) now captures one `ltc-live.bmp` and one `tod-fallback.bmp` from the actual render buffer for offline UX checks.
- Boot-path improvement from ordering trim + service slimming (cold boot sample):
	- boot to first display flip improved from `7.784s` to `4.828s` (~`38%` faster)
	- `ltc-timecode.service` critical-chain activation moved earlier from `@5.447s` to `@2.632s`
- One-line rollback for the service slimming profile:
	- `sudo systemctl enable --now bluetooth.service console-setup.service keyboard-setup.service NetworkManager-wait-online.service udisks2.service e2scrub_reap.service rpi-eeprom-update.service`

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

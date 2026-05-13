# Copilot Instructions — CM5 LTC Viewer (GBM/EGL Embedded Appliance)

## 1. Context & Target Runtime ENVIRONMENT
- **Hardware Profile:** Raspberry Pi Compute Module 5 (CM5) appliance running headless.
- **Operating System:** Raspberry Pi OS Lite (Debian Trixie / 13 Testing) with native `vc4-kms-v3d` full KMS active.
- **Graphics Stack:** Raw, bare-metal DRM/KMS utilizing Generic Buffer Management (GBM) and OpenGL ES 2.0/3.0. No desktop environment, X11, or Wayland.
- **Linked Libraries:** Managed via explicit compilation steps linking `libdrm`, `libgbm`, `libEGL`, `libGLESv2`, `libpng`, `libltc`, and `pthread`.
- **Audio Ingest:** Audio capture handled via ALSA (HiFiBerry target card).

## 2. Codebase Architecture & Boundaries
The project splits backend targets to protect code integrity. Do not breach these boundaries:
- `src/display-backend-gbm.c` & `src/drm.c`: Dedicated strictly to hardware-accelerated GBM allocation, EGL context binding, page-flipping, and DRM display layout orchestration.
- `src/display-backend.c` & `src/font.c`: Handles display abstractions and font/glyph loading. **Constraint:** Font rendering must transition from direct pixel software-blitting into OpenGL ES texturing (GL glyph textures/vbos).
- `src/main.c`: Coordinates thread initialization and real-time capture dispatch loops.
- `src/config-daemon.c` & `src/config-management.c`: Decoupled configuration management handling JSON-based parameters (`/etc/ltc-viewer/config.json`). Completely isolated from rendering files.

## 3. Strict Technical Constraints (Guardrails)
- **Zero Software Blitting:** Do not introduce or fallback onto direct `uint8_t*` buffer manipulations, legacy dumb-buffers, or software framebuffers (`/dev/fb0`). Screen presentation must execute through `eglSwapBuffers()`.
- **Triple-Display Topology:** Ensure concurrent support for 1x DSI panel and 2x HDMI connectors without thread collisions. Implement resource-safe pipelines matching modern `kmscube` multi-display layouts where each connector resolves its own `gbm_surface` and `EGLSurface`.
- **Build Cleanliness:** Maintain compatibility with the unified `Makefile` using individual `pkg-config` queries. Do not add hardcoded library path flags (such as legacy `/opt/vc/*`). Compilations must maintain `-std=c99 -Wall -Wextra -O2`.
- **System Configuration Integrity:** Do not emit code that advises legacy `/boot/firmware/config.txt` interventions (such as `display_rotate` or manual display blanks). Screen orientations or scales must occur entirely within the GPU hardware pipeline using KMS planes or GLES transformations.
- **Memory Hot-Path Safety:** No dynamic allocation (`malloc`, `calloc`, `realloc`) inside the high-frequency render loops (`HH:MM:SS:FF` refresh cadence). Use static, pre-allocated GBM swap chains.

## 4. Prioritised Execution Sequence
1. **GBM/EGL Frame Synchronization:** Fix or tighten page-flipping mechanics to minimize sync-blocking or frame-drop latency when rendering across 3 independent displays simultaneously.
2. **GLES Glyph Pipeline:** Ensure `src/font.c` assets map cleanly into GPU texture buffers instead of software pixel arrays.
3. **Deterministic Threading:** Ensure rendering routines do not block or induce jitter into the high-priority real-time ALSA audio capture thread (`SCHED_FIFO`).
4. **Daemon Integration:** Verify that runtime updates emitted by `ltc-config-daemon` reload gracefully via configuration watches without tearing or resetting the active EGL surface states.

## 5. Execution Protocol (Mandatory on Every Task)
1. **Structural Audit:** Validate matching headers across `include/` and trace buffer dependencies inside `src/display-backend-gbm.c` before outputting code patches.
2. **Surgical Patching:** Provide high-density, strictly localized code blocks. Never restructure surrounding business logic or introduce arbitrary code styling modifications.
3. **Build Validation:** Ensure compliance with the shared compiler variables (`MAIN_CFLAGS`, `COMPILE_CFLAGS`). Every generated loop must cleanly compile without generating compiler warnings.

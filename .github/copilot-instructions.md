# Copilot Instructions — CM5 LTC Viewer (Ultra Compact)

## Scope
CM5 appliance, C/C++ + Makefile, DRM/KMS on DSI, ALSA via HiFiBerry, systemd services, image-based deployment.

## Priorities (strict order)
1. ALSA LTC ingest path
2. Input mode config (Balanced DIFF / Unbalanced SE) + persistence
3. Latency/determinism validation on CM5
4. PREEMPT_RT qualification (only if stable)
5. Docs aligned to image-release workflow

## Guardrails
- Minimal targeted edits.
- Do not regress DSI render/centering.
- Do not reintroduce stale boot guidance (`rotate=90`, forced HDMI disable, hardcoded PARTUUID literals).
- DSI fixed-refresh; HDMI may remain configurable.
- Backward-safe config/schema changes.

## Execution contract (every task)
Inspect → brief plan → focused patch → build/validate → report (status, files changed, verification, risks).

## Validation minimum
`make` passes; services restart; DRM loop healthy; ALSA capture works; config persists; docs match image payload.

## Docs policy
`PRODUCTION_IMAGE_GUIDE.md` = internal canonical. `README.md` = public quick start. No duplicated long procedures.

## Temporary override (explicit only)
For exploratory work, you may temporarily reorder priorities or allow broader refactors **only if declared up front**; then restore defaults after the task.

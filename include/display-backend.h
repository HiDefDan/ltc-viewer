#ifndef DISPLAY_BACKEND_H
#define DISPLAY_BACKEND_H

#include <stdint.h>

#include "drm.h"
#include "display-backend-gbm.h"

typedef enum {
    DISPLAY_BACKEND_DRM = 0,
    DISPLAY_BACKEND_GBM = 1
} display_backend_type_t;

typedef struct {
    display_backend_type_t type;
    union {
        ltc_drm_context_t drm;
        ltc_gbm_context_t gbm;
    } ctx;
} display_backend_t;

int display_backend_init(display_backend_t *backend,
                         uint32_t target_width,
                         uint32_t target_height,
                         float target_hz);

void display_backend_cleanup(display_backend_t *backend);

uint8_t *display_backend_get_back_buffer(display_backend_t *backend);

int display_backend_page_flip_sync(display_backend_t *backend);
int display_backend_render(display_backend_t *backend, const gbm_render_state_t *state);

uint32_t display_backend_back_width(const display_backend_t *backend);
uint32_t display_backend_back_height(const display_backend_t *backend);
uint32_t display_backend_back_pitch(const display_backend_t *backend);
float display_backend_refresh_hz(const display_backend_t *backend);

/* Multi-output API: index 0 is always the primary/permanent display (DSI, or
 * HDMI if no DSI is present); indices >= 1 are hotplugged HDMI outputs.
 * display_backend_output_count() returns the slot high-water mark, not the
 * live output count — a hotplugged-out secondary can leave an inactive gap
 * below a still-active higher index, so callers must check
 * display_backend_output_active() per index rather than assume a compact
 * 0..N-1 range. */
int display_backend_output_count(const display_backend_t *backend);
int display_backend_output_active(const display_backend_t *backend, int idx);
uint32_t display_backend_output_width(const display_backend_t *backend, int idx);
uint32_t display_backend_output_height(const display_backend_t *backend, int idx);
float display_backend_output_refresh_hz(const display_backend_t *backend, int idx);
int display_backend_render_output(display_backend_t *backend, int idx, const gbm_render_state_t *state);
int display_backend_flip_output(display_backend_t *backend, int idx);

/* Rescans connected HDMI outputs and activates/deactivates as needed.
 * Returns the number of outputs that changed state. Cheap to call every
 * frame, but callers should gate it to ~1/sec since it's not needed more
 * often than a cable can physically be moved. */
int display_backend_poll_hotplug(display_backend_t *backend);

/* Drops a secondary output (idx >= 1) after a render/flip failure without
 * touching the rest of the backend. Never call with idx 0. */
void display_backend_deactivate_output(display_backend_t *backend, int idx);

#endif

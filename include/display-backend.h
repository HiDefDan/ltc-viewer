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

#endif

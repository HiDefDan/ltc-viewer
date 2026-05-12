#ifndef DISPLAY_BACKEND_H
#define DISPLAY_BACKEND_H

#include <stdint.h>

#include "drm.h"

typedef struct {
    ltc_drm_context_t drm;
} display_backend_t;

int display_backend_init(display_backend_t *backend,
                         uint32_t target_width,
                         uint32_t target_height,
                         float target_hz);

void display_backend_cleanup(display_backend_t *backend);

uint8_t *display_backend_get_back_buffer(display_backend_t *backend);

int display_backend_page_flip_sync(display_backend_t *backend);

uint32_t display_backend_back_width(const display_backend_t *backend);
uint32_t display_backend_back_height(const display_backend_t *backend);
uint32_t display_backend_back_pitch(const display_backend_t *backend);
float display_backend_refresh_hz(const display_backend_t *backend);

#endif

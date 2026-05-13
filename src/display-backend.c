

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include "display-backend.h"
#include "display-backend-gbm.h"

int display_backend_init(display_backend_t *backend,
                         uint32_t target_width,
                         uint32_t target_height,
                         float target_hz)
{
    if (!backend) {
        return -1;
    }
    memset(backend, 0, sizeof(*backend));
    backend->type = DISPLAY_BACKEND_GBM;
    return gbm_backend_init(&backend->ctx.gbm, target_width, target_height, target_hz);
}

void display_backend_cleanup(display_backend_t *backend)
{
    if (!backend) {
        return;
    }
    if (backend->type == DISPLAY_BACKEND_GBM) {
        gbm_backend_cleanup(&backend->ctx.gbm);
    } else {
        drm_cleanup(&backend->ctx.drm);
    }
}

uint8_t *display_backend_get_back_buffer(display_backend_t *backend)
{
    if (!backend) {
        return NULL;
    }
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_get_back_buffer(&backend->ctx.gbm);
    } else {
        return drm_get_back_buffer(&backend->ctx.drm);
    }
}

int display_backend_page_flip_sync(display_backend_t *backend)
{
    if (!backend) {
        return -1;
    }
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_page_flip_sync(&backend->ctx.gbm);
    } else {
        return drm_page_flip_sync(&backend->ctx.drm);
    }
}

int display_backend_render(display_backend_t *backend, const gbm_render_state_t *state)
{
    if (!backend || !state) {
        return -1;
    }
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_render(&backend->ctx.gbm, state);
    }
    return -1;
}

uint32_t display_backend_back_width(const display_backend_t *backend)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_back_width(&backend->ctx.gbm);
    } else {
        return backend->ctx.drm.back.hdisplay;
    }
}

uint32_t display_backend_back_height(const display_backend_t *backend)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_back_height(&backend->ctx.gbm);
    } else {
        return backend->ctx.drm.back.vdisplay;
    }
}

uint32_t display_backend_back_pitch(const display_backend_t *backend)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_back_pitch(&backend->ctx.gbm);
    } else {
        return backend->ctx.drm.back.pitch;
    }
}

float display_backend_refresh_hz(const display_backend_t *backend)
{
    if (!backend) return 0.0f;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_refresh_hz(&backend->ctx.gbm);
    } else {
        return backend->ctx.drm.mode_refresh_hz;
    }
}

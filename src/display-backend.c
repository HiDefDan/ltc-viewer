

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
        /* Legacy single-output entry point — flips only the primary output.
         * Multi-output callers should use display_backend_flip_output()
         * per active output instead (see main.c's render loop). */
        return gbm_backend_flip_output(&backend->ctx.gbm, 0);
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
        /* Legacy single-output entry point — renders only the primary
         * output. Multi-output callers should use
         * display_backend_render_output() per active output instead. */
        return gbm_backend_render_output(&backend->ctx.gbm, 0, state);
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

int display_backend_output_count(const display_backend_t *backend)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_count(&backend->ctx.gbm);
    }
    return 1;
}

int display_backend_output_active(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_active(&backend->ctx.gbm, idx);
    }
    return idx == 0;
}

uint32_t display_backend_output_width(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_width(&backend->ctx.gbm, idx);
    }
    return idx == 0 ? backend->ctx.drm.back.hdisplay : 0;
}

uint32_t display_backend_output_height(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_height(&backend->ctx.gbm, idx);
    }
    return idx == 0 ? backend->ctx.drm.back.vdisplay : 0;
}

float display_backend_output_refresh_hz(const display_backend_t *backend, int idx)
{
    if (!backend) return 0.0f;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_refresh_hz(&backend->ctx.gbm, idx);
    }
    return idx == 0 ? backend->ctx.drm.mode_refresh_hz : 0.0f;
}

uint32_t display_backend_output_connector_type(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_connector_type(&backend->ctx.gbm, idx);
    }
    return 0;
}

uint32_t display_backend_output_connector_type_id(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_connector_type_id(&backend->ctx.gbm, idx);
    }
    return 0;
}

uint32_t display_backend_output_digit_width(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_digit_width(&backend->ctx.gbm, idx);
    }
    return 0;
}

uint32_t display_backend_output_glyph_height(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_glyph_height(&backend->ctx.gbm, idx);
    }
    return 0;
}

int display_backend_render_output(display_backend_t *backend, int idx, const gbm_render_state_t *state)
{
    if (!backend || !state) return -1;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_render_output(&backend->ctx.gbm, idx, state);
    }
    /* Legacy DRM path never rendered through this abstraction — main.c
     * blits directly into the back buffer for that backend type. */
    return -1;
}

int display_backend_flip_output(display_backend_t *backend, int idx)
{
    if (!backend) return -1;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_flip_output(&backend->ctx.gbm, idx);
    }
    return idx == 0 ? drm_page_flip_sync(&backend->ctx.drm) : -1;
}

int display_backend_flip_output_sync(display_backend_t *backend, int idx)
{
    if (!backend) return -1;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_flip_output_sync(&backend->ctx.gbm, idx);
    }
    return idx == 0 ? drm_page_flip_sync(&backend->ctx.drm) : -1;
}

int display_backend_wait_flips(display_backend_t *backend, int timeout_ms)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_wait_flips(&backend->ctx.gbm, timeout_ms);
    }
    return 0;
}

int display_backend_output_flip_pending(const display_backend_t *backend, int idx)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_output_flip_pending(&backend->ctx.gbm, idx);
    }
    return 0;
}

int display_backend_poll_hotplug(display_backend_t *backend)
{
    if (!backend) return 0;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        return gbm_backend_poll_hotplug(&backend->ctx.gbm);
    }
    return 0;
}

void display_backend_deactivate_output(display_backend_t *backend, int idx)
{
    if (!backend || idx == 0) return;
    if (backend->type == DISPLAY_BACKEND_GBM) {
        gbm_backend_deactivate_output(&backend->ctx.gbm, idx);
    }
}

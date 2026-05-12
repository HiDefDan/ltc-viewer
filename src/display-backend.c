#include "display-backend.h"

int display_backend_init(display_backend_t *backend,
                         uint32_t target_width,
                         uint32_t target_height,
                         float target_hz)
{
    if (!backend) {
        return -1;
    }
    return drm_init(&backend->drm, target_width, target_height, target_hz);
}

void display_backend_cleanup(display_backend_t *backend)
{
    if (!backend) {
        return;
    }
    drm_cleanup(&backend->drm);
}

uint8_t *display_backend_get_back_buffer(display_backend_t *backend)
{
    if (!backend) {
        return NULL;
    }
    return drm_get_back_buffer(&backend->drm);
}

int display_backend_page_flip_sync(display_backend_t *backend)
{
    if (!backend) {
        return -1;
    }
    return drm_page_flip_sync(&backend->drm);
}

uint32_t display_backend_back_width(const display_backend_t *backend)
{
    return backend ? backend->drm.back.hdisplay : 0;
}

uint32_t display_backend_back_height(const display_backend_t *backend)
{
    return backend ? backend->drm.back.vdisplay : 0;
}

uint32_t display_backend_back_pitch(const display_backend_t *backend)
{
    return backend ? backend->drm.back.pitch : 0;
}

float display_backend_refresh_hz(const display_backend_t *backend)
{
    return backend ? backend->drm.mode_refresh_hz : 0.0f;
}

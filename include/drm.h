#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <stddef.h>

/* Avoid namespace collision with system libdrm */
#define DRM_FORMAT_ARGB8888 0x34325241  /* From drm_fourcc.h */

/* DRM framebuffer and display mode */
typedef struct {
    uint32_t fb_id;
    uint32_t hdisplay;
    uint32_t vdisplay;
    uint32_t vrefresh;
    uint32_t pitch;
    uint8_t *buffer;
    size_t buffer_size;
} ltc_drm_framebuffer_t;

typedef struct {
    int fd;                          /* DRM device fd */
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t crtc_index;
    ltc_drm_framebuffer_t front;
    ltc_drm_framebuffer_t back;
    ltc_drm_framebuffer_t current;
    void *current_mode;              /* drmModeModeInfo * (void* to avoid header collision) */
    uint32_t mode_vrefresh;         /* 48, 50, or 60 Hz */
} ltc_drm_context_t;

/* Initialize DRM, find best HDMI mode, set up buffers */
int drm_init(ltc_drm_context_t *ctx, uint32_t target_vrefresh);

/* Clean up DRM resources */
void drm_cleanup(ltc_drm_context_t *ctx);

/* Get the back buffer to draw into */
uint8_t *drm_get_back_buffer(ltc_drm_context_t *ctx);

/* Flip to front buffer, wait for vblank */
int drm_page_flip_sync(ltc_drm_context_t *ctx);

/* Detect actual refresh rate from connector */
uint32_t drm_detect_vrefresh(ltc_drm_context_t *ctx);

#endif /* DRM_H */

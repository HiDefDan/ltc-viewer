#ifndef DISPLAY_BACKEND_GBM_H
#define DISPLAY_BACKEND_GBM_H

#include <stdint.h>
#include <stdbool.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <xf86drmMode.h>

#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int portrait_mode;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t bg_color;
    uint32_t bg_x;
    uint32_t text_x;
    uint32_t text_y;
    float glyph_scale;
    const char *timecode_str;
    uint32_t target_text_color;
    int source_fade_active;
    const char *fade_from_str;
    uint32_t fade_from_color;
    const char *fade_to_str;
    uint32_t fade_to_color;
    int debug_overlay_enabled;
    const char *overlay_str;
    uint32_t overlay_color;
    float overlay_scale;
    int df_flag;
} gbm_render_state_t;

typedef struct {
    struct gbm_device *gbm_dev;
    struct gbm_surface *gbm_surf;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    int drm_fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    uint32_t drm_format;
    struct gbm_bo *prev_bo;
    uint32_t prev_fb;
    uint32_t width, height;
    float refresh_hz;
    uint8_t *back_buffer;
    GLuint back_tex;
    GLuint quad_prog;
    GLuint quad_pos_attr;
    GLuint quad_uv_attr;
} ltc_gbm_context_t;

int gbm_backend_init(ltc_gbm_context_t *ctx, uint32_t width, uint32_t height, float refresh_hz);
void gbm_backend_cleanup(ltc_gbm_context_t *ctx);
uint8_t *gbm_backend_get_back_buffer(ltc_gbm_context_t *ctx);
int gbm_backend_page_flip_sync(ltc_gbm_context_t *ctx);
uint32_t gbm_backend_back_width(const ltc_gbm_context_t *ctx);
uint32_t gbm_backend_back_height(const ltc_gbm_context_t *ctx);
int gbm_backend_render(ltc_gbm_context_t *ctx, const gbm_render_state_t *state);
uint32_t gbm_backend_back_pitch(const ltc_gbm_context_t *ctx);
float gbm_backend_refresh_hz(const ltc_gbm_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_BACKEND_GBM_H

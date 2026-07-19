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

/* One primary (DSI, or HDMI when no DSI is present) plus a handful of
 * simultaneously connected HDMI outputs. Raspberry Pi CM5-class hardware
 * exposes at most a couple of independent HDMI CRTCs, so this is generous
 * headroom rather than a hard platform limit. */
#define LTC_GBM_MAX_OUTPUTS 4

/* Raspberry Pi 5/CM5 splits display scanout across independent DRM devices:
 * DSI is driven by the RP1 southbridge's own KMS driver (drm-rp1-dsi, its
 * own /dev/dri/cardN), while HDMI is driven by the main SoC's vc4-drm on a
 * *different* card. They do not share a GBM device, an EGL display, or a
 * connector/CRTC namespace. This is generous headroom for however many
 * distinct KMS-capable cards a given board exposes. */
#define LTC_GBM_MAX_DEVICES 4

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

/* One GBM device / EGL display per physical DRM device (/dev/dri/cardN)
 * actually used for rendering. Created lazily the first time an output on
 * that card is activated; persists for the process lifetime once created. */
typedef struct {
    int drm_fd;
    struct gbm_device *gbm_dev;
    EGLDisplay egl_display;
    EGLConfig egl_config;
} ltc_gbm_device_t;

/* Per-connector scanout state. Output 0 is always the primary/permanent
 * display (DSI, or HDMI if no DSI is present). Outputs 1..N are HDMI
 * connectors that come and go via hotplug — possibly on a different
 * physical DRM device than output 0 (see drm_fd). Each output carries its
 * own copy of the font atlas texture / glyph shader: outputs on the same
 * drm_fd share one EGL context group (so these are the same GL object names
 * for them), but outputs on a *different* drm_fd cannot share GL objects at
 * all, and need their own atlas/program built in their own context. */
typedef struct {
    int active;
    int drm_fd; /* which ltc_gbm_device_t (by drm_fd) this output scans out on */
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    struct gbm_surface *gbm_surf;
    EGLContext egl_context;
    EGLSurface egl_surface;
    uint32_t drm_format;
    struct gbm_bo *prev_bo;
    uint32_t prev_fb;
    uint32_t width, height;
    float refresh_hz;
    GLuint font_atlas_tex;
    GLuint prog;
    GLint text_color_loc;
    GLint text_tex_loc;
} ltc_gbm_output_t;

/* A known HDMI-type connector, recorded at startup from every DRM device on
 * the system (connected or not), so hotplug polling can notice a connector
 * going from disconnected -> connected even if it wasn't plugged in at
 * process start. `fd` is a raw, always-open dup-free handle to whichever
 * card this connector lives on (shared with that card's ltc_gbm_device_t
 * once/if an output on it is activated). */
typedef struct {
    int fd;
    uint32_t connector_id;
} ltc_gbm_hdmi_candidate_t;

typedef struct {
    ltc_gbm_device_t devices[LTC_GBM_MAX_DEVICES];
    int device_count;
    ltc_gbm_output_t outputs[LTC_GBM_MAX_OUTPUTS];
    int output_count;
    ltc_gbm_hdmi_candidate_t hdmi_candidates[LTC_GBM_MAX_OUTPUTS];
    int hdmi_candidate_count;
    uint8_t *back_buffer; /* legacy accessor support only; not used for GBM rendering */
} ltc_gbm_context_t;

int gbm_backend_init(ltc_gbm_context_t *ctx, uint32_t width, uint32_t height, float refresh_hz);
void gbm_backend_cleanup(ltc_gbm_context_t *ctx);
uint8_t *gbm_backend_get_back_buffer(ltc_gbm_context_t *ctx);

/* Primary-output (index 0) accessors, kept for callers that only care about
 * "the" display (frame pacing, stats logging, config-reload mode changes). */
uint32_t gbm_backend_back_width(const ltc_gbm_context_t *ctx);
uint32_t gbm_backend_back_height(const ltc_gbm_context_t *ctx);
uint32_t gbm_backend_back_pitch(const ltc_gbm_context_t *ctx);
float gbm_backend_refresh_hz(const ltc_gbm_context_t *ctx);

/* Multi-output API. gbm_backend_output_count() returns the slot high-water
 * mark, not the live output count — check gbm_backend_output_active() per
 * index, since a hotplug removal can leave an inactive gap below a still-
 * active higher index. */
int gbm_backend_output_count(const ltc_gbm_context_t *ctx);
int gbm_backend_output_active(const ltc_gbm_context_t *ctx, int idx);
uint32_t gbm_backend_output_width(const ltc_gbm_context_t *ctx, int idx);
uint32_t gbm_backend_output_height(const ltc_gbm_context_t *ctx, int idx);
float gbm_backend_output_refresh_hz(const ltc_gbm_context_t *ctx, int idx);
int gbm_backend_render_output(ltc_gbm_context_t *ctx, int idx, const gbm_render_state_t *state);
int gbm_backend_flip_output(ltc_gbm_context_t *ctx, int idx);

/* Rescan known HDMI connectors (across every DRM device found at startup)
 * for connect/disconnect transitions and activate/deactivate outputs
 * accordingly. Returns the number of outputs that changed state (0 if
 * nothing changed). Safe to call every frame; only does DRM ioctls (cheap)
 * unless a transition is detected. */
int gbm_backend_poll_hotplug(ltc_gbm_context_t *ctx);

/* Deactivate a secondary output (index >= 1) after a render/flip failure,
 * without tearing down the rest of the device. Index 0 (the primary/DSI
 * output) is never torn down this way. */
void gbm_backend_deactivate_output(ltc_gbm_context_t *ctx, int idx);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_BACKEND_GBM_H

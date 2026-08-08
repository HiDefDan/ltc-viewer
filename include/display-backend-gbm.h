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
    /* Always-visible rate/drop-frame indicator (distinct from the debug
     * overlay above). rate_str holds the nominal rate ("24"/"25"/"30"/
     * "29.97"); empty string hides it entirely (e.g. ToD fallback, no
     * lock yet). Positions are pre-computed in build_output_render_state
     * so the backend just draws — rate_x is already the right-justified
     * draw origin, df_x the left-justified one, both anchored on the
     * boundary between the two FF digit cells. */
    char rate_str[12];
    int show_df;
    /* Alpha-packed (top byte) color for the real rate_str/dF glyphs, kept
     * separate from target_text_color: during a source-transition fade this
     * tracks whichever side of the crossfade currently represents live LTC,
     * so the row eases in/out in lockstep with the main timecode instead of
     * snapping at full opacity the instant it appears/disappears. Drawn with
     * use_alpha=1, unlike the dim "88.88"/"dF" background beneath it, which
     * stays a fixed static reference regardless of fade state. */
    uint32_t rate_color;
    uint32_t rate_x, rate_y;
    uint32_t df_x, df_y;
    float rate_scale;
    /* Fixed right-justified position for the dim "88.88" background,
     * independent of the current rate_str's length (see the comment at its
     * computation site in main.c). df's background reuses df_x directly. */
    uint32_t bg_rate_x;
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

#define LTC_GBM_GLYPH_COUNT 13 /* digits 0-9, period, 'd', 'F' (rate/df indicator) */

/* Per-connector scanout state. Output 0 is always the primary/permanent
 * display (DSI, or HDMI if no DSI is present). Outputs 1..N are HDMI
 * connectors that come and go via hotplug — possibly on a different
 * physical DRM device than output 0 (see drm_fd). Every output owns its own
 * font atlas texture, rasterized from the vendored TTF at that output's
 * native glyph pixel size (so the on-screen scale is ~1.0 and no GL_LINEAR
 * resampling occurs) — which is also why the glyph metric/UV tables live
 * here rather than as process globals. The glyph shader *program* is shared
 * between outputs on the same drm_fd (one EGL context group); outputs on a
 * different drm_fd have no shared GL namespace and build their own. */
typedef struct {
    int active;
    int drm_fd; /* which ltc_gbm_device_t (by drm_fd) this output scans out on */
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id; /* per-type instance number, e.g. "1" in HDMI-A-1 */
    uint32_t crtc_id;
    drmModeModeInfo mode;
    struct gbm_surface *gbm_surf;
    EGLContext egl_context;
    EGLSurface egl_surface;
    uint32_t drm_format;
    struct gbm_bo *prev_bo;  /* buffer currently (or last) on scanout */
    struct gbm_bo *next_bo;  /* buffer queued in an in-flight page flip */
    int mode_set;            /* drmModeSetCrtc done once at activation */
    int pending_flip;        /* a drmModePageFlip event is outstanding */
    uint32_t width, height;
    float refresh_hz;
    GLuint font_atlas_tex;
    GLuint prog;
    GLint text_color_loc;
    GLint text_tex_loc;
    float glyph_uv[LTC_GBM_GLYPH_COUNT][4];
    uint32_t glyph_width[LTC_GBM_GLYPH_COUNT];
    uint32_t glyph_height[LTC_GBM_GLYPH_COUNT];
    uint32_t glyph_spacing[LTC_GBM_GLYPH_COUNT];
    uint32_t atlas_digit_w; /* uniform digit cell width in the atlas, px */
    uint32_t atlas_cell_h;  /* uniform glyph cell height in the atlas, px */
    /* Last-logged layout, for the "[GBM] render state" transition log in
     * gbm_render_to_output — per-output (not a shared static) so multiple
     * active outputs with different layouts don't make each other look
     * like they're constantly changing. */
    uint32_t log_text_x, log_text_y, log_logical_w, log_logical_h, log_color;
    float log_glyph_scale;
    int log_portrait;
    int log_valid; /* 0 until the first line has been logged for this output */
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
uint32_t gbm_backend_output_connector_type(const ltc_gbm_context_t *ctx, int idx);
uint32_t gbm_backend_output_connector_type_id(const ltc_gbm_context_t *ctx, int idx);
/* Native glyph metrics of the output's font atlas (px). Layout math in
 * main.c uses these instead of compile-time asset constants. */
uint32_t gbm_backend_output_digit_width(const ltc_gbm_context_t *ctx, int idx);
uint32_t gbm_backend_output_glyph_height(const ltc_gbm_context_t *ctx, int idx);
int gbm_backend_render_output(ltc_gbm_context_t *ctx, int idx, const gbm_render_state_t *state);
/* Debug framegrab: call between render_output and flip_output. Fills buf
 * (width*height*4) with top-down ARGB8888 pixels of the pending frame. */
int gbm_backend_read_pixels(ltc_gbm_context_t *ctx, int idx, uint8_t *buf);

/* Queues an async page flip (drmModePageFlip) of the just-rendered frame.
 * Completion is collected by gbm_backend_wait_flips(); never call again for
 * the same output while gbm_backend_output_flip_pending() is true. Falls
 * back to a blocking SetCrtc if the driver rejects the flip. */
int gbm_backend_flip_output(ltc_gbm_context_t *ctx, int idx);

/* Blocking SetCrtc presentation of the just-rendered frame (drains any
 * in-flight flip first). Used for the final shutdown blank, where the frame
 * must be on scanout before the process exits. */
int gbm_backend_flip_output_sync(ltc_gbm_context_t *ctx, int idx);

/* Waits (up to timeout_ms) for outstanding page flips to complete, i.e.
 * for the vblank that latches each output's queued frame. Returns the
 * number of flips still pending on return (0 = all landed). */
int gbm_backend_wait_flips(ltc_gbm_context_t *ctx, int timeout_ms);

int gbm_backend_output_flip_pending(const ltc_gbm_context_t *ctx, int idx);

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

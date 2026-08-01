#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "display-backend-gbm.h"
#include "font.h"
#include "font-render.h"
#include "config.h"

#define GLYPH_COUNT LTC_GBM_GLYPH_COUNT

/* Padding between atlas cells so GL_LINEAR sampling (especially the ~0.23x
 * minified debug overlay) never bleeds a neighboring glyph's texels in. */
#define ATLAS_CELL_PAD 2
#define ATLAS_ROW0_GLYPHS 6 /* glyphs 0-5 on row 0, 6-9 + period on row 1 */

/* Native glyph pixel height for this output: the TIMECODE_FONT_HEIGHT
 * target, capped so the glyph fits the logical landscape height and the
 * 8-digit string fits the logical width — the same clamp semantics the
 * scale math applied when glyphs were fixed-size assets. Rasterizing at
 * this size makes the on-screen glyph scale ~1.0. */
static uint32_t compute_native_glyph_height(const ltc_gbm_output_t *out) {
    uint32_t render_w = out->width;
    uint32_t render_h = out->height;
    if (render_h > render_w) { /* portrait panel: logical canvas is rotated */
        render_w = out->height;
        render_h = out->width;
    }
    float aspect = font_render_digit_aspect();
    if (aspect <= 0.0f || render_w == 0 || render_h == 0) {
        return 0;
    }
    float px = (float)TIMECODE_FONT_HEIGHT;
    if ((float)render_h < px) {
        px = (float)render_h;
    }
    float max_from_width = (float)render_w / (8.0f * aspect);
    if (max_from_width < px) {
        px = max_from_width;
    }
    if (px < 8.0f) {
        px = 8.0f;
    }
    return (uint32_t)px;
}

/* Rasterizes the glyph set at this output's native pixel size and uploads
 * it as a single-channel (GL_LUMINANCE) atlas texture in whatever EGL
 * context is currently bound, filling the output's metric/UV tables.
 * Every output gets its own atlas: outputs on one DRM device share a GL
 * namespace but can still run different modes, hence different sizes. */
static int build_font_atlas(ltc_gbm_output_t *out) {
    uint32_t px = compute_native_glyph_height(out);
    if (px == 0) {
        fprintf(stderr, "[GBM] Cannot size font atlas for %ux%u output\n", out->width, out->height);
        return -1;
    }

    fr_glyph_set_t set;
    if (font_render_rasterize_set(&set, px) != 0) {
        fprintf(stderr, "[GBM] Font rasterization failed at %u px\n", px);
        return -1;
    }

    uint32_t row_w[2] = {0, 0};
    for (int i = 0; i < GLYPH_COUNT; i++) {
        int r = (i < ATLAS_ROW0_GLYPHS) ? 0 : 1;
        row_w[r] += set.glyphs[i].cell_w + ATLAS_CELL_PAD;
    }
    uint32_t atlas_w = (row_w[0] > row_w[1]) ? row_w[0] : row_w[1];
    uint32_t atlas_h = 2u * (set.cell_h + ATLAS_CELL_PAD);

    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (max_tex > 0 && (atlas_w > (uint32_t)max_tex || atlas_h > (uint32_t)max_tex)) {
        fprintf(stderr, "[GBM] Font atlas %ux%u exceeds GL_MAX_TEXTURE_SIZE %d\n",
                atlas_w, atlas_h, max_tex);
        font_render_free_set(&set);
        return -1;
    }

    uint8_t *atlas = calloc((size_t)atlas_w * atlas_h, 1);
    if (!atlas) {
        font_render_free_set(&set);
        return -1;
    }

    uint32_t x = 0, y = 0;
    for (int i = 0; i < GLYPH_COUNT; i++) {
        if (i == ATLAS_ROW0_GLYPHS) {
            x = 0;
            y = set.cell_h + ATLAS_CELL_PAD;
        }
        const fr_glyph_t *g = &set.glyphs[i];
        for (uint32_t row = 0; row < g->cell_h; row++) {
            memcpy(atlas + (size_t)(y + row) * atlas_w + x,
                   g->bitmap + (size_t)row * g->cell_w, g->cell_w);
        }
        out->glyph_width[i] = g->cell_w;
        out->glyph_height[i] = g->cell_h;
        out->glyph_spacing[i] = g->advance;
        out->glyph_uv[i][0] = (float)x / (float)atlas_w;
        out->glyph_uv[i][1] = (float)y / (float)atlas_h;
        out->glyph_uv[i][2] = (float)(x + g->cell_w) / (float)atlas_w;
        out->glyph_uv[i][3] = (float)(y + g->cell_h) / (float)atlas_h;
        x += g->cell_w + ATLAS_CELL_PAD;
    }
    out->atlas_digit_w = set.digit_cell_w;
    out->atlas_cell_h = set.cell_h;
    font_render_free_set(&set);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, atlas_w, atlas_h, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(atlas);

    fprintf(stderr, "[GBM] font atlas %ux%u (glyph %upx, digit_w %upx) for %ux%u output\n",
            atlas_w, atlas_h, set.cell_h, out->atlas_digit_w, out->width, out->height);
    fflush(stderr);

    out->font_atlas_tex = tex;
    return 0;
}

static const char *vs_src = "attribute vec2 pos; attribute vec2 uv; varying vec2 v_uv; void main() { gl_Position = vec4(pos,0,1); v_uv=uv; }";
static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D tex;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    float alpha = texture2D(tex, v_uv).r;\n" // Use glyph intensity from the gray channel as opacity
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);\n"
    "}";
/* attribute locations are pinned via glBindAttribLocation below, so they're
 * the same constants (0, 1) for every program in every context. */
static const GLuint attr_pos = 0;
static const GLuint attr_uv = 1;

static void setup_shader(GLuint *out_prog, GLint *out_color_loc, GLint *out_tex_loc) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, attr_pos, "pos");
    glBindAttribLocation(prog, attr_uv, "uv");
    glLinkProgram(prog);
    *out_color_loc = glGetUniformLocation(prog, "u_color");
    *out_tex_loc = glGetUniformLocation(prog, "tex");
    glDeleteShader(vs);
    glDeleteShader(fs);
    *out_prog = prog;
}

static int glyph_index(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch == '.') {
        return 10;
    }
    return -1;
}

static void gbm_backend_draw_quad(ltc_gbm_output_t *out,
                                 float x0, float y0,
                                 float x1, float y1,
                                 float u0, float v0,
                                 float u1, float v1,
                                 uint32_t color,
                                 int use_alpha,
                                 int portrait_mode,
                                 uint32_t logical_width,
                                 uint32_t logical_height)
{
    (void)logical_height;
    if (!out || out->width == 0 || out->height == 0) {
        return;
    }

    float phys_x0 = x0;
    float phys_y0 = y0;
    float phys_x1 = x1;
    float phys_y1 = y1;

    if (portrait_mode) {
        phys_x0 = y0;
        phys_y0 = (float)logical_width - x1;
        phys_x1 = y1;
        phys_y1 = (float)logical_width - x0;
    }

    float ndc_x0 = (phys_x0 / (float)out->width) * 2.0f - 1.0f;
    float ndc_y0 = 1.0f - (phys_y0 / (float)out->height) * 2.0f;
    float ndc_x1 = (phys_x1 / (float)out->width) * 2.0f - 1.0f;
    float ndc_y1 = 1.0f - (phys_y1 / (float)out->height) * 2.0f;

    float u00 = u0, v00 = v0;
    float u10 = u1, v10 = v0;
    float u11 = u1, v11 = v1;
    float u01 = u0, v01 = v1;

    if (portrait_mode) {
        // Rotate the glyph texture 90 degrees CCW for portrait display.
        u00 = u1; v00 = v0;
        u10 = u1; v10 = v1;
        u11 = u0; v11 = v1;
        u01 = u0; v01 = v0;
    }

    float verts[16] = {
        ndc_x0, ndc_y0, u00, v00,
        ndc_x1, ndc_y0, u10, v10,
        ndc_x1, ndc_y1, u11, v11,
        ndc_x0, ndc_y1, u01, v01
    };

    uint32_t alpha_key = (color >> 24) & 0xFFu;
    float a = use_alpha ? ((float)alpha_key / 255.0f) : 1.0f;
    float r = ((color >> 16) & 0xFFu) / 255.0f;
    float g = ((color >> 8) & 0xFFu) / 255.0f;
    float b = (color & 0xFFu) / 255.0f;

    if (use_alpha) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glUseProgram(out->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, out->font_atlas_tex);
    glUniform1i(out->text_tex_loc, 0);
    glUniform4f(out->text_color_loc, r, g, b, a);
    glEnableVertexAttribArray(attr_pos);
    glEnableVertexAttribArray(attr_uv);
    glVertexAttribPointer(attr_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(attr_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    if (use_alpha) {
        glDisable(GL_BLEND);
    }
}

static void gbm_backend_draw_text(ltc_gbm_output_t *out,
                                 const char *text,
                                 float x,
                                 float y,
                                 float scale,
                                 uint32_t color,
                                 int use_alpha,
                                 int portrait_mode,
                                 uint32_t logical_width,
                                 uint32_t logical_height)
{
    if (!out || !text || scale <= 0.0f) {
        return;
    }

    float cur_x = x;
    float prev_x = x;
    float glyph_h = (float)out->glyph_height[0] * scale;
    float period_lift = scale + 0.5f;

    for (const char *p = text; *p; p++) {
        int idx = glyph_index(*p);
        if (idx < 0) {
            continue;
        }

        float width = (float)out->glyph_width[idx] * scale;
        float spacing = (float)out->glyph_spacing[idx] * scale;
        if (spacing == 0.0f && out->glyph_spacing[idx] > 0) {
            spacing = 1.0f;
        }

        float draw_x = cur_x;
        float draw_y = y;
        if (*p == '.') {
            draw_x = prev_x;
            draw_y = (y > period_lift) ? (y - period_lift) : y;
        }

        float x0 = draw_x;
        float y0 = draw_y;
        float x1 = draw_x + width;
        float y1 = draw_y + glyph_h;

        gbm_backend_draw_quad(out, x0, y0, x1, y1,
                             out->glyph_uv[idx][0], out->glyph_uv[idx][1],
                             out->glyph_uv[idx][2], out->glyph_uv[idx][3],
                             color, use_alpha,
                             portrait_mode,
                             logical_width, logical_height);

        if (*p != '.') {
            prev_x = cur_x;
        }
        cur_x += spacing;
    }
}

static void gbm_backend_draw_df_badge(ltc_gbm_output_t *out,
                                     uint32_t start_x,
                                     uint32_t start_y,
                                     float scale,
                                     uint32_t color,
                                     int portrait_mode,
                                     uint32_t logical_width,
                                     uint32_t logical_height)
{
    static const int d_pts[][2] = {
        {1,0},{2,0},
        {2,1},
        {1,2},{2,2},
        {1,3},{2,3},
        {1,4},{2,4}
    };
    static const int f_pts[][2] = {
        {0,0},{1,0},{2,0},
        {0,1},
        {0,2},{1,2},
        {0,3},
        {0,4}
    };

    float dot_step = 18.0f * scale;
    if (dot_step < 2.0f) {
        dot_step = 2.0f;
    }

    for (size_t i = 0; i < sizeof(d_pts)/sizeof(d_pts[0]); i++) {
        float px = start_x + d_pts[i][0] * dot_step;
        float py = start_y + d_pts[i][1] * dot_step;
        gbm_backend_draw_text(out, ".", px, py, scale, color, 0, portrait_mode, logical_width, logical_height);
    }

    float fx0 = start_x + 4.0f * dot_step;
    for (size_t i = 0; i < sizeof(f_pts)/sizeof(f_pts[0]); i++) {
        float px = fx0 + f_pts[i][0] * dot_step;
        float py = start_y + f_pts[i][1] * dot_step;
        gbm_backend_draw_text(out, ".", px, py, scale, color, 0, portrait_mode, logical_width, logical_height);
    }
}

static int gbm_render_to_output(ltc_gbm_output_t *out, const gbm_render_state_t *state) {
    static char prev_timecode[64] = {0};
    if (!out || !state || !out->font_atlas_tex || !out->prog) {
        return -1;
    }

    if (state->timecode_str && state->timecode_str[0] &&
        strcmp(prev_timecode, state->timecode_str) != 0) {
        fprintf(stderr, "[GBM] render state: text='%s' color=0x%08X x=%u y=%u scale=%.2f portrait=%d logical=%ux%u\n",
                state->timecode_str,
                state->target_text_color,
                state->text_x,
                state->text_y,
                state->glyph_scale,
                state->portrait_mode,
                state->logical_width,
                state->logical_height);
        fflush(stderr);
        strncpy(prev_timecode, state->timecode_str, sizeof(prev_timecode) - 1);
        prev_timecode[sizeof(prev_timecode) - 1] = '\0';
    }

    glViewport(0, 0, out->width, out->height);
    float bg_r = ((state->bg_color >> 16) & 0xFFu) / 255.0f;
    float bg_g = ((state->bg_color >> 8) & 0xFFu) / 255.0f;
    float bg_b = (state->bg_color & 0xFFu) / 255.0f;
    glClearColor(bg_r, bg_g, bg_b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw background pattern
    const char *bg_pattern = "88.88.88.88.";
    gbm_backend_draw_text(out, bg_pattern,
                         state->bg_x,
                         state->text_y,
                         state->glyph_scale,
                         0xFF0A0A0A,
                         0,
                         state->portrait_mode,
                         state->logical_width,
                         state->logical_height);

    // Draw timecode or fade
    if (state->source_fade_active) {
        if (state->fade_from_str && state->fade_from_color != 0) {
            gbm_backend_draw_text(out, state->fade_from_str,
                                 state->text_x,
                                 state->text_y,
                                 state->glyph_scale,
                                 state->fade_from_color,
                                 1,
                                 state->portrait_mode,
                                 state->logical_width,
                                 state->logical_height);
        }
        if (state->fade_to_str && state->fade_to_color != 0) {
            gbm_backend_draw_text(out, state->fade_to_str,
                                 state->text_x,
                                 state->text_y,
                                 state->glyph_scale,
                                 state->fade_to_color,
                                 1,
                                 state->portrait_mode,
                                 state->logical_width,
                                 state->logical_height);
        }
    } else {
        gbm_backend_draw_text(out, state->timecode_str,
                             state->text_x,
                             state->text_y,
                             state->glyph_scale,
                             state->target_text_color,
                             0,
                             state->portrait_mode,
                             state->logical_width,
                             state->logical_height);
    }

    // Draw overlay if enabled
    if (state->debug_overlay_enabled && state->overlay_str) {
        gbm_backend_draw_text(out, state->overlay_str,
                             12.0f,
                             12.0f,
                             state->overlay_scale,
                             state->overlay_color,
                             0,
                             state->portrait_mode,
                             state->logical_width,
                             state->logical_height);
        if (state->df_flag) {
            float df_x = 12.0f + 16.0f * (float)out->atlas_digit_w * state->overlay_scale;
            gbm_backend_draw_df_badge(out, (uint32_t)df_x, 12u,
                                      state->overlay_scale * 0.72f,
                                      state->overlay_color,
                                      state->portrait_mode,
                                      state->logical_width,
                                      state->logical_height);

        }
    }

    return 0;
}

#define LTC_GBM_MAX_CARD_CONNECTORS 32

typedef struct {
    int fd;
    char path[32];
    uint32_t connector_ids[LTC_GBM_MAX_CARD_CONNECTORS];
    uint32_t connector_types[LTC_GBM_MAX_CARD_CONNECTORS];
    int connector_connected[LTC_GBM_MAX_CARD_CONNECTORS];
    int connector_count;
} drm_card_scan_t;

/* Opens every /dev/dri/cardN that has usable KMS resources (i.e. is an
 * actual display controller, not a render-only node like v3d) and records
 * its connectors. On this platform DSI and HDMI live on different cards
 * entirely, so scanning must not stop at the first working card. */
static int scan_drm_cards(drm_card_scan_t *cards, int max_cards) {
    int count = 0;
    for (int i = 0; i < 16 && count < max_cards; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        drmModeRes *res = drmModeGetResources(fd);
        if (!res || res->count_connectors == 0) {
            if (res) drmModeFreeResources(res);
            close(fd);
            continue;
        }

        drm_card_scan_t *card = &cards[count];
        memset(card, 0, sizeof(*card));
        card->fd = fd;
        snprintf(card->path, sizeof(card->path), "%s", path);

        for (int c = 0; c < res->count_connectors && card->connector_count < LTC_GBM_MAX_CARD_CONNECTORS; c++) {
            drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[c]);
            if (!conn) continue;
            int ci = card->connector_count++;
            card->connector_ids[ci] = conn->connector_id;
            card->connector_types[ci] = conn->connector_type;
            card->connector_connected[ci] = (conn->connection == DRM_MODE_CONNECTED);
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);

        fprintf(stderr, "[GBM] %s: %d connector(s)\n", path, card->connector_count);
        fflush(stderr);
        count++;
    }
    return count;
}

static drmModeModeInfo *find_mode(drmModeConnector *conn, uint32_t target_width, uint32_t target_height, float target_refresh) {
    drmModeModeInfo *best_mode = NULL;
    float best_diff = 1000.0f;

    for (int i = 0; i < conn->count_modes; i++) {
        drmModeModeInfo *mode = &conn->modes[i];
        float actual_refresh = (float)mode->clock * 1000.0f / ((float)mode->htotal * (float)mode->vtotal);
        int exact_resolution = (mode->hdisplay == target_width && mode->vdisplay == target_height);
        float diff = fabsf(actual_refresh - target_refresh);

        if (exact_resolution && diff < 0.02f) {
            return mode;
        }
        if (exact_resolution && diff < best_diff) {
            best_diff = diff;
            best_mode = mode;
        }
    }

    if (best_mode) {
        return best_mode;
    }
    if (conn->count_modes > 0) {
        return &conn->modes[0];
    }
    return NULL;
}

static int crtc_is_excluded(uint32_t crtc_id, const uint32_t *excluded, int excluded_count) {
    for (int i = 0; i < excluded_count; i++) {
        if (excluded[i] == crtc_id) return 1;
    }
    return 0;
}

/* Picks a CRTC for `conn` that isn't already claimed by another active
 * output on the SAME drm fd (CRTC ids are only meaningfully comparable
 * within one DRM device). Tries the connector's currently-bound
 * encoder/CRTC first, then falls back to scanning every encoder's
 * possible_crtcs bitmask. */
static int find_crtc_for_connector(int fd, drmModeConnector *conn,
                                    const uint32_t *excluded, int excluded_count,
                                    uint32_t *crtc_id) {
    if (conn->encoder_id) {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, conn->encoder_id);
        if (encoder) {
            if (encoder->crtc_id && !crtc_is_excluded(encoder->crtc_id, excluded, excluded_count)) {
                *crtc_id = encoder->crtc_id;
                drmModeFreeEncoder(encoder);
                return 0;
            }
            drmModeFreeEncoder(encoder);
        }
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }

    for (int e = 0; e < conn->count_encoders; e++) {
        drmModeEncoder *encoder = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!encoder) continue;
        for (int bit = 0; bit < res->count_crtcs; bit++) {
            if (encoder->possible_crtcs & (1u << bit)) {
                uint32_t candidate = res->crtcs[bit];
                if (!crtc_is_excluded(candidate, excluded, excluded_count)) {
                    *crtc_id = candidate;
                    drmModeFreeEncoder(encoder);
                    drmModeFreeResources(res);
                    return 0;
                }
            }
        }
        drmModeFreeEncoder(encoder);
    }

    drmModeFreeResources(res);
    return -1;
}

static int create_drm_fb_for_bo(ltc_gbm_output_t *out, struct gbm_bo *bo, uint32_t *fb_id) {
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    if (drmModeAddFB2(out->drm_fd, out->width, out->height, out->drm_format, handles, pitches, offsets, fb_id, 0)) {
        fprintf(stderr, "[GBM] drmModeAddFB2 failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* DRM framebuffer id cached on the gbm_bo itself (kmscube pattern): created
 * on the buffer's first trip through the flip path, destroyed automatically
 * with the BO when its gbm_surface is destroyed. The surface cycles through
 * a fixed small set of BOs, so after the first few frames the flip path
 * does no FB creation and no allocation at all. */
typedef struct {
    int drm_fd;
    uint32_t fb_id;
} gbm_fb_cache_t;

static void gbm_fb_cache_destroy(struct gbm_bo *bo, void *data) {
    (void)bo;
    gbm_fb_cache_t *cache = data;
    if (cache) {
        drmModeRmFB(cache->drm_fd, cache->fb_id);
        free(cache);
    }
}

static uint32_t get_fb_for_bo(ltc_gbm_output_t *out, struct gbm_bo *bo) {
    gbm_fb_cache_t *cache = gbm_bo_get_user_data(bo);
    if (cache) {
        return cache->fb_id;
    }
    uint32_t fb_id = 0;
    if (create_drm_fb_for_bo(out, bo, &fb_id) != 0) {
        return 0;
    }
    cache = malloc(sizeof(*cache));
    if (!cache) {
        drmModeRmFB(out->drm_fd, fb_id);
        return 0;
    }
    cache->drm_fd = out->drm_fd;
    cache->fb_id = fb_id;
    gbm_bo_set_user_data(bo, cache, gbm_fb_cache_destroy);
    return fb_id;
}

static void gbm_page_flip_handler(int fd, unsigned int sequence,
                                  unsigned int tv_sec, unsigned int tv_usec,
                                  void *user_data) {
    (void)fd; (void)sequence; (void)tv_sec; (void)tv_usec;
    ltc_gbm_output_t *out = user_data;
    if (!out) return;
    /* The queued buffer is now on scanout; the previously scanned-out
     * buffer is free to be rendered into again. */
    if (out->prev_bo && out->gbm_surf) {
        gbm_surface_release_buffer(out->gbm_surf, out->prev_bo);
    }
    out->prev_bo = out->next_bo;
    out->next_bo = NULL;
    out->pending_flip = 0;
}

int gbm_backend_wait_flips(ltc_gbm_context_t *ctx, int timeout_ms) {
    if (!ctx) return 0;

    drmEventContext ev;
    memset(&ev, 0, sizeof(ev));
    ev.version = 2;
    ev.page_flip_handler = gbm_page_flip_handler;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    deadline.tv_sec += timeout_ms / 1000 + deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;

    for (;;) {
        /* Distinct device fds that still have a flip in flight. */
        struct pollfd pfds[LTC_GBM_MAX_DEVICES];
        int nfds = 0;
        int pending = 0;
        for (int i = 0; i < ctx->output_count; i++) {
            ltc_gbm_output_t *out = &ctx->outputs[i];
            if (!out->active || !out->pending_flip) continue;
            pending++;
            int seen = 0;
            for (int j = 0; j < nfds; j++) {
                if (pfds[j].fd == out->drm_fd) { seen = 1; break; }
            }
            if (!seen && nfds < LTC_GBM_MAX_DEVICES) {
                pfds[nfds].fd = out->drm_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                nfds++;
            }
        }
        if (pending == 0) {
            return 0;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain_ms = (long)(deadline.tv_sec - now.tv_sec) * 1000L
                       + (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remain_ms < 0) {
            return pending;
        }

        int rc = poll(pfds, (nfds_t)nfds, (int)remain_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return pending;
        }
        if (rc == 0) {
            return pending; /* timed out with flips still in flight */
        }
        for (int j = 0; j < nfds; j++) {
            if (pfds[j].revents & POLLIN) {
                drmHandleEvent(pfds[j].fd, &ev);
            }
        }
    }
}

int gbm_backend_output_flip_pending(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].active && ctx->outputs[idx].pending_flip;
}

/* Finds (or lazily creates) the ltc_gbm_device_t for a given drm fd. Devices
 * are never destroyed except in gbm_backend_cleanup, so once created a
 * device's gbm_dev/egl_display stay valid for the process lifetime even if
 * every output on it is later deactivated. */
static int get_or_create_device(ltc_gbm_context_t *ctx, int fd) {
    for (int i = 0; i < ctx->device_count; i++) {
        if (ctx->devices[i].drm_fd == fd) {
            return i;
        }
    }
    if (ctx->device_count >= LTC_GBM_MAX_DEVICES) {
        fprintf(stderr, "[GBM] Too many distinct DRM devices in use\n"); fflush(stderr);
        return -1;
    }

    ltc_gbm_device_t *dev = &ctx->devices[ctx->device_count];
    memset(dev, 0, sizeof(*dev));
    dev->drm_fd = fd;

    dev->gbm_dev = gbm_create_device(fd);
    if (!dev->gbm_dev) {
        fprintf(stderr, "[GBM] gbm_create_device failed for fd %d\n", fd); fflush(stderr);
        return -1;
    }

    dev->egl_display = eglGetDisplay((EGLNativeDisplayType)dev->gbm_dev);
    if (dev->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[GBM] eglGetDisplay failed for fd %d\n", fd); fflush(stderr);
        gbm_device_destroy(dev->gbm_dev);
        return -1;
    }
    if (!eglInitialize(dev->egl_display, NULL, NULL)) {
        fprintf(stderr, "[GBM] eglInitialize failed for fd %d, eglGetError=0x%x\n", fd, eglGetError()); fflush(stderr);
        gbm_device_destroy(dev->gbm_dev);
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint num_configs;
    if (!eglChooseConfig(dev->egl_display, config_attribs, &dev->egl_config, 1, &num_configs) || num_configs < 1) {
        fprintf(stderr, "[GBM] eglChooseConfig failed for fd %d, eglGetError=0x%x\n", fd, eglGetError()); fflush(stderr);
        eglTerminate(dev->egl_display);
        gbm_device_destroy(dev->gbm_dev);
        return -1;
    }

    int idx = ctx->device_count++;
    fprintf(stderr, "[GBM] Opened DRM device fd=%d as device[%d]\n", fd, idx); fflush(stderr);
    return idx;
}

/* Activates output `idx` on connector `connector_id`, which lives on `fd`.
 * If another active output already exists on the same fd, the new EGL
 * context shares that output's context (so the same GL object names are
 * valid) and the font atlas/shader are copied rather than rebuilt. Outputs
 * on a different fd cannot share anything and get their own fresh atlas. */
static int activate_output(ltc_gbm_context_t *ctx, int idx, int fd, uint32_t connector_id,
                            uint32_t target_width, uint32_t target_height, float target_refresh) {
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    memset(out, 0, sizeof(*out));

    int dev_idx = get_or_create_device(ctx, fd);
    if (dev_idx < 0) {
        return -1;
    }
    ltc_gbm_device_t *dev = &ctx->devices[dev_idx];

    drmModeConnector *conn = drmModeGetConnector(fd, connector_id);
    if (!conn) {
        fprintf(stderr, "[GBM] Failed to get connector %u\n", connector_id); fflush(stderr);
        return -1;
    }
    if (conn->connection != DRM_MODE_CONNECTED) {
        drmModeFreeConnector(conn);
        return -1;
    }
    out->drm_fd = fd;
    out->connector_id = connector_id;
    out->connector_type = conn->connector_type;
    out->connector_type_id = conn->connector_type_id;

    drmModeModeInfo *mode = find_mode(conn, target_width, target_height, target_refresh);
    if (!mode) {
        fprintf(stderr, "[GBM] Failed to find display mode for connector %u\n", connector_id); fflush(stderr);
        drmModeFreeConnector(conn);
        return -1;
    }
    out->mode = *mode;
    out->width = out->mode.hdisplay;
    out->height = out->mode.vdisplay;
    out->refresh_hz = target_refresh > 0.0f ? target_refresh : (float)out->mode.vrefresh;
    if (target_width && target_height &&
        ((uint32_t)out->mode.hdisplay != target_width || (uint32_t)out->mode.vdisplay != target_height)) {
        fprintf(stderr, "[GBM] Requested %ux%u does not match chosen mode %ux%u on connector %u, using mode size\n",
                target_width, target_height, out->mode.hdisplay, out->mode.vdisplay, connector_id);
        fflush(stderr);
    }

    uint32_t excluded[LTC_GBM_MAX_OUTPUTS];
    int excluded_count = 0;
    ltc_gbm_output_t *sibling = NULL;
    for (int i = 0; i < ctx->output_count; i++) {
        if (ctx->outputs[i].active && ctx->outputs[i].drm_fd == fd) {
            excluded[excluded_count++] = ctx->outputs[i].crtc_id;
            if (!sibling) {
                sibling = &ctx->outputs[i];
            }
        }
    }

    if (find_crtc_for_connector(fd, conn, excluded, excluded_count, &out->crtc_id) != 0) {
        fprintf(stderr, "[GBM] Failed to find a free CRTC for connector %u\n", connector_id); fflush(stderr);
        drmModeFreeConnector(conn);
        return -1;
    }
    drmModeFreeConnector(conn);

    out->gbm_surf = gbm_surface_create(dev->gbm_dev, out->width, out->height, GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (out->gbm_surf) {
        out->drm_format = DRM_FORMAT_ARGB8888;
    } else {
        fprintf(stderr, "[GBM] gbm_surface_create ARGB8888 failed for connector %u, trying XRGB8888\n", connector_id); fflush(stderr);
        out->gbm_surf = gbm_surface_create(dev->gbm_dev, out->width, out->height, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!out->gbm_surf) {
            fprintf(stderr, "[GBM] gbm_surface_create XRGB8888 also failed for connector %u\n", connector_id); fflush(stderr);
            return -1;
        }
        out->drm_format = DRM_FORMAT_XRGB8888;
    }

    EGLContext share_ctx = sibling ? sibling->egl_context : EGL_NO_CONTEXT;
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    out->egl_context = eglCreateContext(dev->egl_display, dev->egl_config, share_ctx, ctx_attribs);
    if (out->egl_context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GBM] eglCreateContext failed for connector %u, eglGetError=0x%x\n", connector_id, eglGetError()); fflush(stderr);
        gbm_surface_destroy(out->gbm_surf);
        out->gbm_surf = NULL;
        return -1;
    }

    out->egl_surface = eglCreateWindowSurface(dev->egl_display, dev->egl_config, (EGLNativeWindowType)out->gbm_surf, NULL);
    if (out->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[GBM] eglCreateWindowSurface failed for connector %u, eglGetError=0x%x\n", connector_id, eglGetError()); fflush(stderr);
        eglDestroyContext(dev->egl_display, out->egl_context);
        out->egl_context = EGL_NO_CONTEXT;
        gbm_surface_destroy(out->gbm_surf);
        out->gbm_surf = NULL;
        return -1;
    }

    if (!eglMakeCurrent(dev->egl_display, out->egl_surface, out->egl_surface, out->egl_context)) {
        fprintf(stderr, "[GBM] eglMakeCurrent failed for connector %u, eglGetError=0x%x\n", connector_id, eglGetError()); fflush(stderr);
        eglDestroySurface(dev->egl_display, out->egl_surface);
        eglDestroyContext(dev->egl_display, out->egl_context);
        out->egl_surface = EGL_NO_SURFACE;
        out->egl_context = EGL_NO_CONTEXT;
        gbm_surface_destroy(out->gbm_surf);
        out->gbm_surf = NULL;
        return -1;
    }

    /* Presentation timing is owned by drmModePageFlip; eglSwapBuffers must
     * never add its own vsync throttle on top. */
    eglSwapInterval(dev->egl_display, 0);

    if (sibling) {
        /* Same drm fd as an existing output: shares this context's object
         * namespace, so the sibling's program name is valid here too. */
        out->prog = sibling->prog;
        out->text_color_loc = sibling->text_color_loc;
        out->text_tex_loc = sibling->text_tex_loc;
    } else {
        setup_shader(&out->prog, &out->text_color_loc, &out->text_tex_loc);
    }

    /* Atlas is always per-output: it's rasterized at this output's native
     * mode size, which a same-device sibling running a different mode
     * cannot share. */
    if (build_font_atlas(out) != 0) {
        fprintf(stderr, "[GBM] Failed to build font atlas for connector %u\n", connector_id); fflush(stderr);
        eglDestroySurface(dev->egl_display, out->egl_surface);
        eglDestroyContext(dev->egl_display, out->egl_context);
        out->egl_surface = EGL_NO_SURFACE;
        out->egl_context = EGL_NO_CONTEXT;
        gbm_surface_destroy(out->gbm_surf);
        out->gbm_surf = NULL;
        return -1;
    }

    if (!sibling) {
        glViewport(0, 0, out->width, out->height);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        eglSwapBuffers(dev->egl_display, out->egl_surface);
    }

    out->active = 1;
    printf("[GBM] Output %d activated: fd=%d connector=%u type=%u crtc=%u mode=%ux%u@%.2f\n",
           idx, fd, out->connector_id, out->connector_type, out->crtc_id, out->width, out->height, out->refresh_hz);
    fflush(stdout);
    return 0;
}

void gbm_backend_deactivate_output(ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= LTC_GBM_MAX_OUTPUTS) return;
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    if (!out->active) return;

    /* Safe to destroy this output's EGL context even if a sibling on the
     * same drm fd shares its object namespace: EGL/GLES share groups keep
     * shared objects alive as long as at least one context in the group
     * still exists, independent of creation order. */
    int dev_idx = -1;
    for (int i = 0; i < ctx->device_count; i++) {
        if (ctx->devices[i].drm_fd == out->drm_fd) { dev_idx = i; break; }
    }

    /* Let an in-flight flip land before tearing the surface down (its
     * completion handler touches out->prev_bo/next_bo). On timeout we
     * proceed anyway — destroying the surface releases the BOs and their
     * cached FBs regardless. */
    if (out->pending_flip) {
        gbm_backend_wait_flips(ctx, 100);
    }
    if (out->next_bo && out->gbm_surf) {
        gbm_surface_release_buffer(out->gbm_surf, out->next_bo);
    }
    if (out->prev_bo && out->gbm_surf) {
        gbm_surface_release_buffer(out->gbm_surf, out->prev_bo);
    }
    if (dev_idx >= 0 && ctx->devices[dev_idx].egl_display != EGL_NO_DISPLAY) {
        EGLDisplay dpy = ctx->devices[dev_idx].egl_display;
        /* The atlas texture is owned exclusively by this output, but a
         * sibling context in the same share group would keep it alive after
         * this context dies — delete it explicitly. The program stays: it
         * may be shared with a surviving sibling. */
        if (out->font_atlas_tex &&
            out->egl_context != EGL_NO_CONTEXT && out->egl_surface != EGL_NO_SURFACE &&
            eglMakeCurrent(dpy, out->egl_surface, out->egl_surface, out->egl_context)) {
            glDeleteTextures(1, &out->font_atlas_tex);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        if (out->egl_surface != EGL_NO_SURFACE) eglDestroySurface(dpy, out->egl_surface);
        if (out->egl_context != EGL_NO_CONTEXT) eglDestroyContext(dpy, out->egl_context);
    }
    if (out->gbm_surf) gbm_surface_destroy(out->gbm_surf);

    printf("[GBM] Output %d deactivated (connector=%u)\n", idx, out->connector_id);
    fflush(stdout);
    memset(out, 0, sizeof(*out));
}

int gbm_backend_init(ltc_gbm_context_t *ctx, uint32_t width, uint32_t height, float refresh_hz) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    drm_card_scan_t cards[LTC_GBM_MAX_DEVICES];
    int card_count = scan_drm_cards(cards, LTC_GBM_MAX_DEVICES);
    if (card_count == 0) {
        fprintf(stderr, "[GBM] No usable DRM display devices found\n"); fflush(stderr);
        return -1;
    }

    int primary_card = -1;
    uint32_t primary_connector = 0;
    int primary_is_dsi = 0;

    for (int i = 0; i < card_count && primary_card < 0; i++) {
        for (int c = 0; c < cards[i].connector_count; c++) {
            if (cards[i].connector_types[c] == DRM_MODE_CONNECTOR_DSI && cards[i].connector_connected[c]) {
                primary_card = i;
                primary_connector = cards[i].connector_ids[c];
                primary_is_dsi = 1;
                fprintf(stderr, "[GBM] Using DSI connector=%u on %s as primary output\n", primary_connector, cards[i].path);
                break;
            }
        }
    }
    if (primary_card < 0) {
        for (int i = 0; i < card_count && primary_card < 0; i++) {
            for (int c = 0; c < cards[i].connector_count; c++) {
                uint32_t t = cards[i].connector_types[c];
                if ((t == DRM_MODE_CONNECTOR_HDMIA || t == DRM_MODE_CONNECTOR_HDMIB) && cards[i].connector_connected[c]) {
                    primary_card = i;
                    primary_connector = cards[i].connector_ids[c];
                    primary_is_dsi = 0;
                    fprintf(stderr, "[GBM] No DSI connector; using HDMI connector=%u on %s as primary output\n", primary_connector, cards[i].path);
                    break;
                }
            }
        }
    }
    fflush(stderr);

    if (primary_card < 0) {
        fprintf(stderr, "[GBM] No connected DSI or HDMI connector found\n"); fflush(stderr);
        for (int i = 0; i < card_count; i++) close(cards[i].fd);
        return -1;
    }

    /* Record every HDMI-type connector on every card (connected or not) so
     * hotplug polling can find them later, then close any card fd that
     * isn't the primary and has no HDMI connector at all (nothing further
     * to do with it). */
    for (int i = 0; i < card_count; i++) {
        int keep = (i == primary_card);
        for (int c = 0; c < cards[i].connector_count; c++) {
            uint32_t t = cards[i].connector_types[c];
            if (t == DRM_MODE_CONNECTOR_HDMIA || t == DRM_MODE_CONNECTOR_HDMIB) {
                if (ctx->hdmi_candidate_count < LTC_GBM_MAX_OUTPUTS) {
                    ctx->hdmi_candidates[ctx->hdmi_candidate_count].fd = cards[i].fd;
                    ctx->hdmi_candidates[ctx->hdmi_candidate_count].connector_id = cards[i].connector_ids[c];
                    ctx->hdmi_candidate_count++;
                }
                keep = 1;
            }
        }
        if (!keep) {
            close(cards[i].fd);
        }
    }

    uint32_t want_w = primary_is_dsi ? (width ? width : 480) : 0;
    uint32_t want_h = primary_is_dsi ? (height ? height : 1920) : 0;
    float want_hz = primary_is_dsi ? (refresh_hz > 0.0f ? refresh_hz : 60.0f) : 0.0f;

    if (activate_output(ctx, 0, cards[primary_card].fd, primary_connector, want_w, want_h, want_hz) != 0) {
        gbm_backend_cleanup(ctx);
        return -1;
    }
    ctx->output_count = 1;

    ctx->back_buffer = malloc((size_t)ctx->outputs[0].width * (size_t)ctx->outputs[0].height * 4u);
    if (!ctx->back_buffer) {
        fprintf(stderr, "[GBM] Failed to allocate back buffer\n"); fflush(stderr);
        gbm_backend_cleanup(ctx);
        return -1;
    }

    fprintf(stderr, "[GBM] GBM/EGL/OpenGL ES backend initialized: primary %ux%u@%.2f (%d HDMI connector(s) known)\n",
            ctx->outputs[0].width, ctx->outputs[0].height, ctx->outputs[0].refresh_hz, ctx->hdmi_candidate_count); fflush(stderr);
    return 0;
}

void gbm_backend_cleanup(ltc_gbm_context_t *ctx) {
    if (!ctx) return;

    for (int i = 0; i < ctx->output_count; i++) {
        if (ctx->outputs[i].active) {
            gbm_backend_deactivate_output(ctx, i);
        }
    }

    /* Collect every fd worth closing exactly once: device fds (which own an
     * EGL display to terminate) plus any HDMI-candidate fd that never got
     * promoted to a device (never had an output activated on it). */
    int closed_fds[LTC_GBM_MAX_DEVICES + LTC_GBM_MAX_OUTPUTS];
    int closed_count = 0;

    for (int i = 0; i < ctx->device_count; i++) {
        ltc_gbm_device_t *dev = &ctx->devices[i];
        if (dev->egl_display != EGL_NO_DISPLAY) {
            eglTerminate(dev->egl_display);
        }
        if (dev->gbm_dev) {
            gbm_device_destroy(dev->gbm_dev);
        }
        closed_fds[closed_count++] = dev->drm_fd;
        close(dev->drm_fd);
    }

    for (int i = 0; i < ctx->hdmi_candidate_count; i++) {
        int fd = ctx->hdmi_candidates[i].fd;
        int already_closed = 0;
        for (int j = 0; j < closed_count; j++) {
            if (closed_fds[j] == fd) { already_closed = 1; break; }
        }
        if (!already_closed) {
            close(fd);
            closed_fds[closed_count++] = fd;
        }
    }

    if (ctx->back_buffer) {
        free(ctx->back_buffer);
    }
    memset(ctx, 0, sizeof(*ctx));
}

uint8_t *gbm_backend_get_back_buffer(ltc_gbm_context_t *ctx) {
    if (!ctx) return NULL;
    return ctx->back_buffer;
}

int gbm_backend_render_output(ltc_gbm_context_t *ctx, int idx, const gbm_render_state_t *state) {
    if (!ctx || idx < 0 || idx >= ctx->output_count || !ctx->outputs[idx].active) {
        return -1;
    }
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < ctx->device_count; i++) {
        if (ctx->devices[i].drm_fd == out->drm_fd) { dpy = ctx->devices[i].egl_display; break; }
    }
    if (!eglMakeCurrent(dpy, out->egl_surface, out->egl_surface, out->egl_context)) {
        fprintf(stderr, "[GBM] eglMakeCurrent failed for output %d render, eglGetError=0x%x\n", idx, eglGetError());
        fflush(stderr);
        return -1;
    }
    return gbm_render_to_output(out, state);
}

/* Debug-only framegrab support: reads back the just-rendered (not yet
 * swapped) frame as ARGB8888 rows top-down into buf (width*height*4 bytes).
 * Must be called after gbm_backend_render_output and before the flip. */
int gbm_backend_read_pixels(ltc_gbm_context_t *ctx, int idx, uint8_t *buf) {
    if (!ctx || !buf || idx < 0 || idx >= ctx->output_count || !ctx->outputs[idx].active) {
        return -1;
    }
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    /* Context is already current from the render call in this iteration. */
    glReadPixels(0, 0, (GLsizei)out->width, (GLsizei)out->height,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf);
    if (glGetError() != GL_NO_ERROR) {
        return -1;
    }
    /* GL rows are bottom-up RGBA; convert in place to top-down ARGB8888
     * byte order (B,G,R,A) as dump_render_buffer_bmp expects. */
    uint32_t row_bytes = out->width * 4u;
    for (uint32_t y = 0; y < out->height / 2 + (out->height & 1u); y++) {
        uint8_t *top = buf + (size_t)y * row_bytes;
        uint8_t *bot = buf + (size_t)(out->height - 1 - y) * row_bytes;
        for (uint32_t x = 0; x < row_bytes; x += 4) {
            uint8_t tr = top[x], tg = top[x + 1], tb = top[x + 2], ta = top[x + 3];
            if (top != bot) {
                top[x] = bot[x + 2]; top[x + 1] = bot[x + 1]; top[x + 2] = bot[x]; top[x + 3] = bot[x + 3];
                bot[x] = tb; bot[x + 1] = tg; bot[x + 2] = tr; bot[x + 3] = ta;
            } else {
                top[x] = tb; top[x + 2] = tr;
            }
        }
    }
    return 0;
}

/* Common front half of both flip flavors: swap the EGL surface and lock
 * the freshly rendered front buffer, returning it with its cached FB id. */
static struct gbm_bo *lock_rendered_frame(ltc_gbm_context_t *ctx, int idx, uint32_t *fb_id) {
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < ctx->device_count; i++) {
        if (ctx->devices[i].drm_fd == out->drm_fd) { dpy = ctx->devices[i].egl_display; break; }
    }

    if (!eglMakeCurrent(dpy, out->egl_surface, out->egl_surface, out->egl_context)) {
        fprintf(stderr, "[GBM] eglMakeCurrent failed for output %d flip, eglGetError=0x%x\n", idx, eglGetError());
        fflush(stderr);
        return NULL;
    }

    if (!eglSwapBuffers(dpy, out->egl_surface)) {
        fprintf(stderr, "[GBM] eglSwapBuffers failed for output %d, eglGetError=0x%x\n", idx, eglGetError()); fflush(stderr);
        return NULL;
    }

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(out->gbm_surf);
    if (!bo) {
        fprintf(stderr, "[GBM] gbm_surface_lock_front_buffer failed for output %d\n", idx); fflush(stderr);
        return NULL;
    }

    *fb_id = get_fb_for_bo(out, bo);
    if (*fb_id == 0) {
        gbm_surface_release_buffer(out->gbm_surf, bo);
        return NULL;
    }
    return bo;
}

/* Blocking present: full modeset onto the new buffer, returns with the
 * frame on scanout. Also the first-frame path (the one legitimate modeset)
 * and the fallback when the driver rejects async flips. */
static int present_sync(ltc_gbm_output_t *out, struct gbm_bo *bo, uint32_t fb_id) {
    if (drmModeSetCrtc(out->drm_fd, out->crtc_id, fb_id, 0, 0, &out->connector_id, 1, &out->mode) != 0) {
        fprintf(stderr, "[GBM] drmModeSetCrtc failed for output (crtc=%u connector=%u): %s\n",
                out->crtc_id, out->connector_id, strerror(errno)); fflush(stderr);
        gbm_surface_release_buffer(out->gbm_surf, bo);
        return -1;
    }
    out->mode_set = 1;
    if (out->prev_bo) {
        gbm_surface_release_buffer(out->gbm_surf, out->prev_bo);
    }
    out->prev_bo = bo;
    return 0;
}

int gbm_backend_flip_output(ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count || !ctx->outputs[idx].active) {
        return -1;
    }
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    if (out->pending_flip) {
        /* Caller must wait for the outstanding flip first. */
        return -1;
    }

    uint32_t fb_id = 0;
    struct gbm_bo *bo = lock_rendered_frame(ctx, idx, &fb_id);
    if (!bo) {
        return -1;
    }

    /* First frame after activation needs the one legitimate modeset to
     * bind CRTC->connector; every later frame rides the vblank via an
     * async page flip. */
    if (!out->mode_set) {
        return present_sync(out, bo, fb_id);
    }

    if (drmModePageFlip(out->drm_fd, out->crtc_id, fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, out) == 0) {
        out->next_bo = bo;
        out->pending_flip = 1;
        return 0;
    }

    if (errno == EBUSY) {
        /* A flip is somehow still in flight (shouldn't happen given the
         * pending_flip gate) — drop this frame rather than block. */
        fprintf(stderr, "[GBM] drmModePageFlip EBUSY on output %d, dropping frame\n", idx); fflush(stderr);
        gbm_surface_release_buffer(out->gbm_surf, bo);
        return 0;
    }

    /* Driver refused the flip (e.g. EINVAL) — fall back to the blocking
     * modeset so the display keeps working, and log once per output. */
    static int logged_fallback[LTC_GBM_MAX_OUTPUTS] = {0};
    if (idx >= 0 && idx < LTC_GBM_MAX_OUTPUTS && !logged_fallback[idx]) {
        fprintf(stderr, "[GBM] drmModePageFlip failed on output %d (%s), using SetCrtc fallback\n",
                idx, strerror(errno)); fflush(stderr);
        logged_fallback[idx] = 1;
    }
    return present_sync(out, bo, fb_id);
}

int gbm_backend_flip_output_sync(ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count || !ctx->outputs[idx].active) {
        return -1;
    }
    ltc_gbm_output_t *out = &ctx->outputs[idx];
    if (out->pending_flip) {
        gbm_backend_wait_flips(ctx, 100);
    }
    uint32_t fb_id = 0;
    struct gbm_bo *bo = lock_rendered_frame(ctx, idx, &fb_id);
    if (!bo) {
        return -1;
    }
    return present_sync(out, bo, fb_id);
}

int gbm_backend_poll_hotplug(ltc_gbm_context_t *ctx) {
    if (!ctx) return 0;
    int changes = 0;

    for (int h = 0; h < ctx->hdmi_candidate_count; h++) {
        int fd = ctx->hdmi_candidates[h].fd;
        uint32_t connector_id = ctx->hdmi_candidates[h].connector_id;

        int existing_idx = -1;
        for (int i = 0; i < ctx->output_count; i++) {
            if (ctx->outputs[i].active && ctx->outputs[i].drm_fd == fd &&
                ctx->outputs[i].connector_id == connector_id) {
                existing_idx = i;
                break;
            }
        }

        drmModeConnector *conn = drmModeGetConnector(fd, connector_id);
        int connected = conn && conn->connection == DRM_MODE_CONNECTED;
        if (conn) drmModeFreeConnector(conn);

        if (existing_idx < 0 && connected) {
            int slot = -1;
            for (int i = 0; i < LTC_GBM_MAX_OUTPUTS; i++) {
                if (i >= ctx->output_count || !ctx->outputs[i].active) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                continue; /* all output slots in use */
            }
            if (activate_output(ctx, slot, fd, connector_id, 0, 0, 0) == 0) {
                if (slot == ctx->output_count) {
                    ctx->output_count++;
                }
                changes++;
            }
        } else if (existing_idx > 0 && !connected) {
            /* Output 0 (the permanent primary) is never torn down here. */
            gbm_backend_deactivate_output(ctx, existing_idx);
            changes++;
        }
    }

    return changes;
}

/* Returns the slot high-water mark (0..output_count-1 are valid indices to
 * query), NOT the number of currently-active outputs — a secondary output
 * can be deactivated (hotplug removal) leaving an inactive gap below a
 * still-active higher index, so callers must check gbm_backend_output_active
 * per slot rather than assume a compact 0..N-1 range of live outputs. */
int gbm_backend_output_count(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->output_count : 0;
}

int gbm_backend_output_active(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].active;
}

uint32_t gbm_backend_output_width(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].width;
}

uint32_t gbm_backend_output_height(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].height;
}

float gbm_backend_output_refresh_hz(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0.0f;
    return ctx->outputs[idx].refresh_hz;
}

uint32_t gbm_backend_output_connector_type(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].connector_type;
}

uint32_t gbm_backend_output_connector_type_id(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].connector_type_id;
}

uint32_t gbm_backend_output_digit_width(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].atlas_digit_w;
}

uint32_t gbm_backend_output_glyph_height(const ltc_gbm_context_t *ctx, int idx) {
    if (!ctx || idx < 0 || idx >= ctx->output_count) return 0;
    return ctx->outputs[idx].atlas_cell_h;
}

uint32_t gbm_backend_back_width(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->outputs[0].width : 480;
}

uint32_t gbm_backend_back_height(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->outputs[0].height : 1920;
}

uint32_t gbm_backend_back_pitch(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->outputs[0].width * 4 : 480 * 4;
}

float gbm_backend_refresh_hz(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->outputs[0].refresh_hz : 60.0f;
}

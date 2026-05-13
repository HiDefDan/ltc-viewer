#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <png.h>
#include <errno.h>
#include <math.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "display-backend-gbm.h"
#include "font.h"
#include "config.h"

// Font atlas state
#define GLYPH_COUNT 11
static GLuint font_atlas_tex = 0;
static GLint text_color_loc = -1;
static GLint text_tex_loc = -1;
static float glyph_uv[GLYPH_COUNT][4];
static uint32_t glyph_width[GLYPH_COUNT];
static uint32_t glyph_height[GLYPH_COUNT];
static uint32_t glyph_spacing[GLYPH_COUNT];

static uint8_t *load_png_rgba(const char *filename, uint32_t *w, uint32_t *h) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    png_init_io(png, fp);
    png_read_info(png, info);
    *w = png_get_image_width(png, info);
    *h = png_get_image_height(png, info);

    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);
    int has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) || png_get_valid(png, info, PNG_INFO_tRNS);

    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png);
        has_alpha = 1;
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (!has_alpha) {
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    }
    png_set_strip_16(png);
    png_set_packing(png);
    png_read_update_info(png, info);
    size_t rowbytes = png_get_rowbytes(png, info);
    uint8_t *pixels = malloc(rowbytes * (*h));
    if (!pixels) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    png_bytep *rows = malloc(sizeof(png_bytep) * (*h));
    if (!rows) { free(pixels); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    for (uint32_t y = 0; y < *h; y++) rows[y] = pixels + y * rowbytes;
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return pixels;
}

static const char *glyphs_dir(void) {
    static const char *installed = "/usr/share/ltc-timecode/glyphs";
    static const char *source = "data/glyphs";
    if (access(installed, R_OK) == 0) {
        return installed;
    }
    return source;
}

static int build_font_atlas(void) {
    const char *glyphs[GLYPH_COUNT] = {"0","1","2","3","4","5","6","7","8","9","period"};
    const char *dir = glyphs_dir();
    uint32_t gw[GLYPH_COUNT] = {0}, gh[GLYPH_COUNT] = {0};
    uint8_t *gimg[GLYPH_COUNT] = {0};
    uint32_t total_w = 0, max_h = 0;
    for (int i = 0; i < GLYPH_COUNT; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.png", dir, glyphs[i]);
        gimg[i] = load_png_rgba(path, &gw[i], &gh[i]);
        if (!gimg[i]) {
            fprintf(stderr, "[GBM] Failed to load glyph image: %s\n", path);
            for (int j = 0; j < i; j++) free(gimg[j]);
            return -1;
        }
        total_w += gw[i];
        if (gh[i] > max_h) max_h = gh[i];
    }
    uint8_t *atlas = calloc(total_w * max_h, 4);
    if (!atlas) {
        for (int i = 0; i < GLYPH_COUNT; i++) free(gimg[i]);
        return -1;
    }
    uint32_t x = 0;
    for (int i = 0; i < GLYPH_COUNT; i++) {
        for (uint32_t row = 0; row < gh[i]; row++) {
            memcpy(atlas + 4 * (x + row * total_w), gimg[i] + 4 * (row * gw[i]), 4 * gw[i]);
        }
        glyph_width[i] = gw[i];
        glyph_height[i] = gh[i];
        glyph_spacing[i] = (i == 10) ? 0 : gw[i];
        glyph_uv[i][0] = (float)x / (float)total_w;
        glyph_uv[i][1] = 0.0f;
        glyph_uv[i][2] = (float)(x + gw[i]) / (float)total_w;
        glyph_uv[i][3] = (float)gh[i] / (float)max_h;
        x += gw[i];
        free(gimg[i]);
    }
    uint8_t min_alpha = 255;
    uint8_t max_alpha = 0;
    uint64_t alpha_sum = 0;

    glGenTextures(1, &font_atlas_tex);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, total_w, max_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (uint32_t i = 3; i < total_w * max_h * 4; i += 4) {
        uint8_t a = atlas[i];
        if (a < min_alpha) min_alpha = a;
        if (a > max_alpha) max_alpha = a;
        alpha_sum += a;
    }
    fprintf(stderr, "[GBM] font atlas alpha range [%u..%u] avg=%.1f\n",
            min_alpha, max_alpha, (double)alpha_sum / (double)(total_w * max_h));
    fflush(stderr);

    free(atlas);
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
static GLuint prog = 0, attr_pos = 0, attr_uv = 0;

static GLuint setup_backbuffer_shader(void) {
    return 0;
}

static void setup_shader(void) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "uv");
    glLinkProgram(prog);
    attr_pos = 0;
    attr_uv = 1;
    text_color_loc = glGetUniformLocation(prog, "u_color");
    text_tex_loc = glGetUniformLocation(prog, "tex");
    glDeleteShader(vs);
    glDeleteShader(fs);
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

static void gbm_backend_draw_quad(ltc_gbm_context_t *ctx,
                                 float x0, float y0,
                                 float x1, float y1,
                                 float u0, float v0,
                                 float u1, float v1,
                                 uint32_t color,
                                 int portrait_mode,
                                 uint32_t logical_width,
                                 uint32_t logical_height)
{
    (void)logical_width;
    if (!ctx || ctx->width == 0 || ctx->height == 0) {
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

    float ndc_x0 = (phys_x0 / (float)ctx->width) * 2.0f - 1.0f;
    float ndc_y0 = 1.0f - (phys_y0 / (float)ctx->height) * 2.0f;
    float ndc_x1 = (phys_x1 / (float)ctx->width) * 2.0f - 1.0f;
    float ndc_y1 = 1.0f - (phys_y1 / (float)ctx->height) * 2.0f;

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
    float a = (alpha_key == 0u) ? 1.0f : ((float)alpha_key / 255.0f);
    float r = ((color >> 16) & 0xFFu) / 255.0f;
    float g = ((color >> 8) & 0xFFu) / 255.0f;
    float b = (color & 0xFFu) / 255.0f;

    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font_atlas_tex);
    glUniform1i(text_tex_loc, 0);
    glUniform4f(text_color_loc, r, g, b, a);
    glEnableVertexAttribArray(attr_pos);
    glEnableVertexAttribArray(attr_uv);
    glVertexAttribPointer(attr_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(attr_uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

static void gbm_backend_draw_text(ltc_gbm_context_t *ctx,
                                 const char *text,
                                 float x,
                                 float y,
                                 float scale,
                                 uint32_t color,
                                 int portrait_mode,
                                 uint32_t logical_width,
                                 uint32_t logical_height)
{
    if (!ctx || !text || scale <= 0.0f) {
        return;
    }

    float cur_x = x;
    float prev_x = x;
    float glyph_h = (float)glyph_height[0] * scale;
    float period_lift = scale + 0.5f;

    for (const char *p = text; *p; p++) {
        int idx = glyph_index(*p);
        if (idx < 0) {
            continue;
        }

        float width = (float)glyph_width[idx] * scale;
        float spacing = (float)glyph_spacing[idx] * scale;
        if (spacing == 0.0f && glyph_spacing[idx] > 0) {
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

        gbm_backend_draw_quad(ctx, x0, y0, x1, y1,
                             glyph_uv[idx][0], glyph_uv[idx][1],
                             glyph_uv[idx][2], glyph_uv[idx][3],
                             color, portrait_mode,
                             logical_width, logical_height);

        if (*p != '.') {
            prev_x = cur_x;
        }
        cur_x += spacing;
    }
}

static void gbm_backend_draw_df_badge(ltc_gbm_context_t *ctx,
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
        gbm_backend_draw_text(ctx, ".", px, py, scale, color, portrait_mode, logical_width, logical_height);
    }

    float fx0 = start_x + 4.0f * dot_step;
    for (size_t i = 0; i < sizeof(f_pts)/sizeof(f_pts[0]); i++) {
        float px = fx0 + f_pts[i][0] * dot_step;
        float py = start_y + f_pts[i][1] * dot_step;
        gbm_backend_draw_text(ctx, ".", px, py, scale, color, portrait_mode, logical_width, logical_height);
    }
}

int gbm_backend_render(ltc_gbm_context_t *ctx, const gbm_render_state_t *state) {
    static char prev_timecode[64] = {0};
    if (!ctx || !state || !ctx->egl_display || !ctx->egl_surface || !font_atlas_tex || !prog) {
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

    glViewport(0, 0, ctx->width, ctx->height);
    float bg_r = ((state->bg_color >> 16) & 0xFFu) / 255.0f;
    float bg_g = ((state->bg_color >> 8) & 0xFFu) / 255.0f;
    float bg_b = (state->bg_color & 0xFFu) / 255.0f;
    glClearColor(bg_r, bg_g, bg_b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw background pattern
    const char *bg_pattern = "8.8.8.8.8.8.8.8.";
    gbm_backend_draw_text(ctx, bg_pattern,
                         state->bg_x,
                         state->text_y,
                         state->glyph_scale,
                         0xFF0A0A0A,
                         state->portrait_mode,
                         state->logical_width,
                         state->logical_height);

    // Draw timecode or fade
    if (state->source_fade_active) {
        if (state->fade_from_str && state->fade_from_color != 0) {
            gbm_backend_draw_text(ctx, state->fade_from_str,
                                 state->text_x,
                                 state->text_y,
                                 state->glyph_scale,
                                 state->fade_from_color,
                                 state->portrait_mode,
                                 state->logical_width,
                                 state->logical_height);
        }
        if (state->fade_to_str && state->fade_to_color != 0) {
            gbm_backend_draw_text(ctx, state->fade_to_str,
                                 state->text_x,
                                 state->text_y,
                                 state->glyph_scale,
                                 state->fade_to_color,
                                 state->portrait_mode,
                                 state->logical_width,
                                 state->logical_height);
        }
    } else {
        gbm_backend_draw_text(ctx, state->timecode_str,
                             state->text_x,
                             state->text_y,
                             state->glyph_scale,
                             state->target_text_color,
                             state->portrait_mode,
                             state->logical_width,
                             state->logical_height);
    }

    // Draw overlay if enabled
    if (state->debug_overlay_enabled && state->overlay_str) {
        gbm_backend_draw_text(ctx, state->overlay_str,
                             12.0f,
                             12.0f,
                             state->overlay_scale,
                             state->overlay_color,
                             state->portrait_mode,
                             state->logical_width,
                             state->logical_height);
        if (state->df_flag) {
            float df_x = 12.0f + 16.0f * (float)DISPLAY_DIGIT_WIDTH * state->overlay_scale;
            gbm_backend_draw_df_badge(ctx, (uint32_t)df_x, 12u,
                                      state->overlay_scale * 0.72f,
                                      state->overlay_color,
                                      state->portrait_mode,
                                      state->logical_width,
                                      state->logical_height);
        }
    }

    return 0;
}

static int open_drm_device(void) {
    for (int i = 0; i < 16; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            drmModeRes *res = drmModeGetResources(fd);
            if (res) {
                drmModeFreeResources(res);
                return fd;
            }
            close(fd);
        }
    }
    return -1;
}

static int find_display_connector(int fd, uint32_t *connector_id) {
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        return -1;
    }

    uint32_t dsi_id = 0;
    uint32_t hdmi_id = 0;
    int dsi_idx = -1, hdmi_idx = -1, dsi_type = 0, hdmi_type = 0;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;

        if (conn->connection == DRM_MODE_CONNECTED) {
            if (conn->connector_type == DRM_MODE_CONNECTOR_DSI && !dsi_id) {
                dsi_id = res->connectors[i];
                dsi_idx = i;
                dsi_type = conn->connector_type;
            } else if ((conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
                        conn->connector_type == DRM_MODE_CONNECTOR_HDMIB) && !hdmi_id) {
                hdmi_id = res->connectors[i];
                hdmi_idx = i;
                hdmi_type = conn->connector_type;
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);

    if (dsi_id) {
        *connector_id = dsi_id;
        fprintf(stderr, "[GBM] Using DSI connector=%u (index=%d type=%d)\n", dsi_id, dsi_idx, dsi_type);
        fflush(stderr);
        return 0;
    }
    if (hdmi_id) {
        *connector_id = hdmi_id;
        fprintf(stderr, "[GBM] Using HDMI connector=%u (index=%d type=%d)\n", hdmi_id, hdmi_idx, hdmi_type);
        fflush(stderr);
        return 0;
    }
    return -1;
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

static int find_crtc_for_connector(int fd, drmModeConnector *conn, uint32_t *crtc_id) {
    drmModeEncoder *encoder = NULL;
    if (conn->encoder_id) {
        encoder = drmModeGetEncoder(fd, conn->encoder_id);
    }
    if (!encoder) {
        for (int i = 0; i < conn->count_encoders; i++) {
            encoder = drmModeGetEncoder(fd, conn->encoders[i]);
            if (encoder) break;
        }
    }
    if (!encoder) {
        return -1;
    }

    if (encoder->crtc_id) {
        *crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
        return 0;
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        drmModeFreeEncoder(encoder);
        return -1;
    }

    for (int bit = 0; bit < res->count_crtcs; bit++) {
        if (encoder->possible_crtcs & (1 << bit)) {
            *crtc_id = res->crtcs[bit];
            drmModeFreeEncoder(encoder);
            drmModeFreeResources(res);
            return 0;
        }
    }

    drmModeFreeEncoder(encoder);
    drmModeFreeResources(res);
    return -1;
}


static int create_drm_fb_for_bo(ltc_gbm_context_t *ctx, struct gbm_bo *bo, uint32_t *fb_id) {
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t width = ctx->width;
    uint32_t height = ctx->height;
    uint32_t format = ctx->drm_format;
    uint32_t handles[4] = { handle, 0, 0, 0 };
    uint32_t pitches[4] = { stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    if (drmModeAddFB2(ctx->drm_fd, width, height, format, handles, pitches, offsets, fb_id, 0)) {
        fprintf(stderr, "[GBM] drmModeAddFB2 failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int gbm_backend_init(ltc_gbm_context_t *ctx, uint32_t width, uint32_t height, float refresh_hz) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));
    int drm_fd = open_drm_device();
    if (drm_fd < 0) {
        fprintf(stderr, "[GBM] Failed to open DRM device\n"); fflush(stderr);
        return -1;
    }
    ctx->drm_fd = drm_fd;
    if (find_display_connector(drm_fd, &ctx->connector_id) != 0) {
        fprintf(stderr, "[GBM] No connected DSI/HDMI connector found\n"); fflush(stderr);
        close(drm_fd);
        return -1;
    }

    ctx->gbm_dev = gbm_create_device(drm_fd);
    if (!ctx->gbm_dev) {
        fprintf(stderr, "[GBM] gbm_create_device failed\n"); fflush(stderr);
        close(drm_fd);
        return -1;
    }
    ctx->width = width ? width : 480;
    ctx->height = height ? height : 1920;
    ctx->refresh_hz = refresh_hz > 0.0f ? refresh_hz : 60.0f;

    drmModeConnector *conn = drmModeGetConnector(drm_fd, ctx->connector_id);
    if (!conn) {
        fprintf(stderr, "[GBM] Failed to get connector %u\n", ctx->connector_id); fflush(stderr);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    drmModeModeInfo *mode = find_mode(conn, ctx->width, ctx->height, ctx->refresh_hz);
    if (!mode) {
        fprintf(stderr, "[GBM] Failed to find display mode for %ux%u@%.2f\n", ctx->width, ctx->height, ctx->refresh_hz); fflush(stderr);
        drmModeFreeConnector(conn);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    ctx->mode = *mode;
    if ((uint32_t)ctx->mode.hdisplay != ctx->width || (uint32_t)ctx->mode.vdisplay != ctx->height) {
        fprintf(stderr, "[GBM] Requested %ux%u does not match chosen mode %ux%u, using mode size\n",
                width ? width : 480, height ? height : 1920,
                ctx->mode.hdisplay, ctx->mode.vdisplay);
        fflush(stderr);
        ctx->width = ctx->mode.hdisplay;
        ctx->height = ctx->mode.vdisplay;
    }
    if (find_crtc_for_connector(drm_fd, conn, &ctx->crtc_id) != 0) {
        fprintf(stderr, "[GBM] Failed to find CRTC for connector %u\n", ctx->connector_id); fflush(stderr);
        drmModeFreeConnector(conn);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    printf("[GBM] Selected connector=%u crtc=%u mode=%ux%u@%.2f\n",
           ctx->connector_id, ctx->crtc_id, ctx->mode.hdisplay, ctx->mode.vdisplay,
           (float)ctx->mode.vrefresh);
    fflush(stdout);
    drmModeFreeConnector(conn);

    ctx->gbm_surf = gbm_surface_create(ctx->gbm_dev, ctx->width, ctx->height, GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (ctx->gbm_surf) {
        ctx->drm_format = DRM_FORMAT_ARGB8888;
    } else {
        fprintf(stderr, "[GBM] gbm_surface_create ARGB8888 failed, trying XRGB8888\n"); fflush(stderr);
        ctx->gbm_surf = gbm_surface_create(ctx->gbm_dev, ctx->width, ctx->height, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!ctx->gbm_surf) {
            fprintf(stderr, "[GBM] gbm_surface_create XRGB8888 also failed\n"); fflush(stderr);
            gbm_device_destroy(ctx->gbm_dev);
            close(drm_fd);
            return -1;
        }
        ctx->drm_format = DRM_FORMAT_XRGB8888;
    }

    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->gbm_dev);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "[GBM] eglGetDisplay failed\n"); fflush(stderr);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    if (!eglInitialize(ctx->egl_display, NULL, NULL)) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglInitialize failed, eglGetError=0x%x\n", err); fflush(stderr);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
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
    EGLConfig egl_config;
    EGLint num_configs;
    EGLBoolean choose_ok = eglChooseConfig(ctx->egl_display, config_attribs, &egl_config, 1, &num_configs);
    if (!choose_ok || num_configs < 1) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglChooseConfig failed, eglGetError=0x%x\n", err); fflush(stderr);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ctx->egl_context = eglCreateContext(ctx->egl_display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglCreateContext failed, eglGetError=0x%x\n", err); fflush(stderr);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, egl_config, (EGLNativeWindowType)ctx->gbm_surf, NULL);
    if (ctx->egl_surface == EGL_NO_SURFACE) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglCreateWindowSurface failed, eglGetError=0x%x\n", err); fflush(stderr);
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    if (!eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context)) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglMakeCurrent failed, eglGetError=0x%x\n", err); fflush(stderr);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    ctx->back_buffer = malloc((size_t)ctx->width * (size_t)ctx->height * 4u);
    if (!ctx->back_buffer) {
        fprintf(stderr, "[GBM] Failed to allocate back buffer\n"); fflush(stderr);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    glGenTextures(1, &ctx->back_tex);
    glBindTexture(GL_TEXTURE_2D, ctx->back_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ctx->width, ctx->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    ctx->quad_prog = setup_backbuffer_shader();
    ctx->quad_pos_attr = 0;
    ctx->quad_uv_attr = 1;
    if (build_font_atlas() != 0) {
        fprintf(stderr, "[GBM] Failed to build font atlas\n"); fflush(stderr);
        if (ctx->quad_prog) glDeleteProgram(ctx->quad_prog);
        if (ctx->back_tex) glDeleteTextures(1, &ctx->back_tex);
        free(ctx->back_buffer);
        eglDestroySurface(ctx->egl_display, ctx->egl_surface);
        eglDestroyContext(ctx->egl_display, ctx->egl_context);
        eglTerminate(ctx->egl_display);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        close(drm_fd);
        return -1;
    }
    setup_shader();
    glViewport(0, 0, ctx->width, ctx->height);
    glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(ctx->egl_display, ctx->egl_surface);
    fprintf(stderr, "[GBM] GBM/EGL/OpenGL ES backend initialized: %ux%u@%.2f\n", ctx->width, ctx->height, ctx->refresh_hz); fflush(stderr);
    return 0;
}

void gbm_backend_cleanup(ltc_gbm_context_t *ctx) {
    if (!ctx) return;
    if (ctx->egl_display && ctx->egl_surface) eglDestroySurface(ctx->egl_display, ctx->egl_surface);
    if (ctx->egl_display && ctx->egl_context) eglDestroyContext(ctx->egl_display, ctx->egl_context);
    if (ctx->egl_display) eglTerminate(ctx->egl_display);
    if (ctx->back_tex) glDeleteTextures(1, &ctx->back_tex);
    if (font_atlas_tex) {
        glDeleteTextures(1, &font_atlas_tex);
        font_atlas_tex = 0;
    }
    if (ctx->quad_prog) glDeleteProgram(ctx->quad_prog);
    if (prog) {
        glDeleteProgram(prog);
        prog = 0;
    }
    if (ctx->back_buffer) free(ctx->back_buffer);
    if (ctx->gbm_surf) gbm_surface_destroy(ctx->gbm_surf);
    if (ctx->gbm_dev) {
        int fd = gbm_device_get_fd(ctx->gbm_dev);
        gbm_device_destroy(ctx->gbm_dev);
        if (fd >= 0) close(fd);
    }
    memset(ctx, 0, sizeof(*ctx));
}

uint8_t *gbm_backend_get_back_buffer(ltc_gbm_context_t *ctx) {
    if (!ctx) return NULL;
    return ctx->back_buffer;
}

int gbm_backend_page_flip_sync(ltc_gbm_context_t *ctx) {
    if (!ctx || !ctx->egl_display || !ctx->egl_surface) {
        fprintf(stderr, "[GBM] Page flip invalid GBM context\n"); fflush(stderr);
        return -1;
    }

    if (!eglSwapBuffers(ctx->egl_display, ctx->egl_surface)) {
        EGLint err = eglGetError();
        fprintf(stderr, "[GBM] eglSwapBuffers failed, eglGetError=0x%x\n", err); fflush(stderr);
        return -1;
    }

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(ctx->gbm_surf);
    if (!bo) {
        fprintf(stderr, "[GBM] gbm_surface_lock_front_buffer failed\n"); fflush(stderr);
        return -1;
    }

    uint32_t fb_id = 0;
    if (create_drm_fb_for_bo(ctx, bo, &fb_id) != 0) {
        gbm_surface_release_buffer(ctx->gbm_surf, bo);
        return -1;
    }

    fprintf(stderr, "[GBM] Presenting FB %u on CRTC %u connector %u\n", fb_id, ctx->crtc_id, ctx->connector_id);
    fflush(stderr);

    if (drmModeSetCrtc(ctx->drm_fd, ctx->crtc_id, fb_id, 0, 0, &ctx->connector_id, 1, &ctx->mode) != 0) {
        fprintf(stderr, "[GBM] drmModeSetCrtc failed: %s\n", strerror(errno)); fflush(stderr);
        drmModeRmFB(ctx->drm_fd, fb_id);
        gbm_surface_release_buffer(ctx->gbm_surf, bo);
        return -1;
    }

    if (ctx->prev_fb) {
        drmModeRmFB(ctx->drm_fd, ctx->prev_fb);
    }
    if (ctx->prev_bo) {
        gbm_surface_release_buffer(ctx->gbm_surf, ctx->prev_bo);
    }
    ctx->prev_bo = bo;
    ctx->prev_fb = fb_id;

    return 0;
}

uint32_t gbm_backend_back_width(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->width : 480;
}

uint32_t gbm_backend_back_height(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->height : 1920;
}

uint32_t gbm_backend_back_pitch(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->width * 4 : 480 * 4;
}

float gbm_backend_refresh_hz(const ltc_gbm_context_t *ctx) {
    return ctx ? ctx->refresh_hz : 60.0f;
}

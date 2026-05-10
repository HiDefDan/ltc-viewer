#include "font.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <png.h>

/* 7-segment font loader from individual PNG files
   Each glyph 0-9 is 204x256, period is 214x256
   Period overlaps 5px on each side of adjacent digits
*/

#define DSEG7_DIR "/usr/share/ltc-timecode/glyphs/"
#define PERIOD_OVERLAP_PX 5
#define PERIOD_WIDTH 214
#define PERIOD_HEIGHT 256
#define DIGIT_WIDTH 209
#define DIGIT_HEIGHT 256
#define FONT_ALPHA_GAMMA 0.90f

/* Individual glyph storage */
typedef struct {
    char ch;
    uint32_t width;
    uint32_t height;
    uint32_t spacing_width;  /* How many pixels to advance cursor */
    uint8_t *pixels;         /* Grayscale pixel data */
    size_t pixels_size;
} glyph_t;

typedef struct {
    int valid;
    char ch;
    uint32_t fg_color;
    uint8_t fg_alpha;
    uint32_t scale_key;
    uint32_t scaled_w;
    uint32_t scaled_h;
    uint8_t *alpha_map;      /* scaled_w * scaled_h alpha values */
    uint16_t *row_start;     /* first non-zero alpha x for each row */
    uint16_t *row_end;       /* one-past-last non-zero alpha x for each row */
    uint64_t last_used_tick;
} glyph_cache_entry_t;

static glyph_t glyph_map[11] = {0};  /* 0-9 + period */
static glyph_cache_entry_t glyph_cache[64] = {0};
static uint64_t glyph_cache_tick = 0;
static uint8_t alpha_gamma_lut[256] = {0};
static int alpha_gamma_lut_ready = 0;

static void init_alpha_gamma_lut(void) {
    if (alpha_gamma_lut_ready) {
        return;
    }

    for (int i = 0; i < 256; i++) {
        float n = (float)i / 255.0f;
        float g = powf(n, FONT_ALPHA_GAMMA);
        int v = (int)lroundf(g * 255.0f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        alpha_gamma_lut[i] = (uint8_t)v;
    }
    alpha_gamma_lut_ready = 1;
}

static void free_cache_entry(glyph_cache_entry_t *entry) {
    if (!entry) {
        return;
    }
    free(entry->alpha_map);
    free(entry->row_start);
    free(entry->row_end);
    memset(entry, 0, sizeof(*entry));
}

static int load_png_from_file(const char *filename, uint8_t **pixels_out, 
                              uint32_t *width, uint32_t *height) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "[FONT] Failed to open %s\n", filename);
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    *width = png_get_image_width(png, info);
    *height = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_RGB_ALPHA)
        png_set_rgb_to_gray(png, 1, 0.299, 0.587);

    /* Strip alpha channel to get single-byte grayscale */
    png_set_strip_alpha(png);

    png_read_update_info(png, info);

    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * (*height));
    for (uint32_t y = 0; y < *height; y++) {
        row_pointers[y] = (png_byte *)malloc(png_get_rowbytes(png, info));
    }

    png_read_image(png, row_pointers);

    size_t total_size = (*width) * (*height);
    *pixels_out = (uint8_t *)malloc(total_size);
    for (uint32_t y = 0; y < *height; y++) {
        memcpy(*pixels_out + y * (*width), row_pointers[y], (*width));
        free(row_pointers[y]);
    }
    free(row_pointers);

    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    return 0;
}

int font_init(void) {
    fflush(stdout);

    init_alpha_gamma_lut();

    for (size_t i = 0; i < (sizeof(glyph_cache) / sizeof(glyph_cache[0])); i++) {
        free_cache_entry(&glyph_cache[i]);
    }

    /* Load digits 0-9 */
    const char *digit_chars = "0123456789";
    for (int i = 0; i < 10; i++) {
        char filename[256];
        snprintf(filename, sizeof(filename), "%s%c.png", DSEG7_DIR, digit_chars[i]);

        glyph_map[i].ch = digit_chars[i];
        glyph_map[i].width = DIGIT_WIDTH;
        glyph_map[i].height = DIGIT_HEIGHT;
        glyph_map[i].spacing_width = DIGIT_WIDTH;  /* 204px advance */

        if (load_png_from_file(filename, &glyph_map[i].pixels,
                              &glyph_map[i].width, &glyph_map[i].height) != 0) {
            fprintf(stderr, "[FONT] Failed to load digit %c\n", digit_chars[i]);
            return -1;
        }
        glyph_map[i].pixels_size = glyph_map[i].width * glyph_map[i].height;
    }

    /* Load period */
    char period_filename[256];
    snprintf(period_filename, sizeof(period_filename), "%speriod.png", DSEG7_DIR);

    glyph_map[10].ch = '.';
    glyph_map[10].width = PERIOD_WIDTH;
    glyph_map[10].height = PERIOD_HEIGHT;
    glyph_map[10].spacing_width = 0;  /* Period overlays, no cursor advance */

    if (load_png_from_file(period_filename, &glyph_map[10].pixels,
                          &glyph_map[10].width, &glyph_map[10].height) != 0) {
        fprintf(stderr, "[FONT] Failed to load period\n");
        return -1;
    }
    glyph_map[10].pixels_size = glyph_map[10].width * glyph_map[10].height;

    printf("[FONT] Glyphs loaded\n");
    fflush(stdout);
    return 0;
}

void font_load_background(void) {
    /* Background uses glyph rendering, no separate PNG loading needed */
}

void font_blit_background(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                          uint32_t x, uint32_t y) {
    font_blit_background_scaled(fb, fb_width, fb_height, fb_pitch, x, y, 1.0f);
}

void font_blit_background_scaled(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                 uint32_t x, uint32_t y, float scale) {
    /* Render "8.8.8.8.8.8.8.8." - subtle dark gray visible on black canvas */
    uint32_t glyph_color = 0xFF0A0A0A;   /* RGB(10, 10, 10) - dark plastic, subtle but visible */
    const char *bg_pattern = "8.8.8.8.8.8.8.8.";

    font_blit_string_scaled(fb, fb_width, fb_height, fb_pitch,
                            x, y, bg_pattern,
                            glyph_color, scale);
}

static glyph_t *font_find_glyph(char ch) {
    for (int i = 0; i < 11; i++) {
        if (glyph_map[i].ch == ch) {
            return &glyph_map[i];
        }
    }
    return NULL;
}

static glyph_cache_entry_t *font_get_cached_glyph(char ch, uint32_t fg_color, float scale) {
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    glyph_t *glyph = font_find_glyph(ch);
    if (!glyph || !glyph->pixels) {
        return NULL;
    }

    uint32_t scale_key = (uint32_t)(scale * 10000.0f + 0.5f);
    glyph_cache_tick++;

    glyph_cache_entry_t *free_slot = NULL;
    glyph_cache_entry_t *lru_slot = &glyph_cache[0];
    for (size_t i = 0; i < (sizeof(glyph_cache) / sizeof(glyph_cache[0])); i++) {
        glyph_cache_entry_t *entry = &glyph_cache[i];
        if (!entry->valid) {
            if (!free_slot) {
                free_slot = entry;
            }
            continue;
        }

        if (entry->ch == ch && entry->fg_color == fg_color && entry->scale_key == scale_key) {
            entry->last_used_tick = glyph_cache_tick;
            return entry;
        }

        if (entry->last_used_tick < lru_slot->last_used_tick) {
            lru_slot = entry;
        }
    }

    glyph_cache_entry_t *entry = free_slot ? free_slot : lru_slot;
    free_cache_entry(entry);

    entry->scaled_w = (uint32_t)(glyph->width * scale + 0.5f);
    entry->scaled_h = (uint32_t)(glyph->height * scale + 0.5f);
    if (entry->scaled_w == 0 || entry->scaled_h == 0) {
        return NULL;
    }

    size_t map_size = (size_t)entry->scaled_w * (size_t)entry->scaled_h;
    entry->alpha_map = (uint8_t *)calloc(map_size, sizeof(uint8_t));
    if (!entry->alpha_map) {
        free_cache_entry(entry);
        return NULL;
    }

    entry->row_start = (uint16_t *)calloc((size_t)entry->scaled_h, sizeof(uint16_t));
    entry->row_end = (uint16_t *)calloc((size_t)entry->scaled_h, sizeof(uint16_t));
    if (!entry->row_start || !entry->row_end) {
        free_cache_entry(entry);
        return NULL;
    }

    for (uint32_t row = 0; row < entry->scaled_h; row++) {
        int has_alpha = 0;
        uint32_t first = 0;
        uint32_t last = 0;

        uint32_t src_row = (uint32_t)(row / scale);
        if (src_row >= glyph->height) {
            src_row = glyph->height - 1;
        }

        for (uint32_t col = 0; col < entry->scaled_w; col++) {
            uint32_t src_col = (uint32_t)(col / scale);
            if (src_col >= glyph->width) {
                src_col = glyph->width - 1;
            }

            uint8_t gray_val = glyph->pixels[src_row * glyph->width + src_col];
            uint8_t gamma_alpha = alpha_gamma_lut[gray_val];
            entry->alpha_map[(size_t)row * entry->scaled_w + col] = gamma_alpha;
            if (gamma_alpha > 0) {
                if (!has_alpha) {
                    first = col;
                    has_alpha = 1;
                }
                last = col + 1;
            }
        }

        if (has_alpha) {
            entry->row_start[row] = (uint16_t)first;
            entry->row_end[row] = (uint16_t)last;
        } else {
            entry->row_start[row] = 0;
            entry->row_end[row] = 0;
        }
    }

    entry->ch = ch;
    entry->fg_color = fg_color & 0x00FFFFFFu;
    entry->fg_alpha = (uint8_t)((fg_color >> 24) & 0xFFu);
    if (entry->fg_alpha == 0) {
        entry->fg_alpha = 0xFFu;
    }
    entry->scale_key = scale_key;
    entry->last_used_tick = glyph_cache_tick;
    entry->valid = 1;
    return entry;
}

static inline uint32_t blend_over_rgb(uint32_t bg_color, uint32_t fg_rgb, uint8_t alpha) {
    if (alpha == 255) {
        return 0xFF000000u | fg_rgb;
    }
    if (alpha == 0) {
        return bg_color;
    }

    uint32_t inv = 255u - (uint32_t)alpha;

    uint32_t bg_r = (bg_color >> 16) & 0xFFu;
    uint32_t bg_g = (bg_color >> 8) & 0xFFu;
    uint32_t bg_b = bg_color & 0xFFu;

    uint32_t fg_r = (fg_rgb >> 16) & 0xFFu;
    uint32_t fg_g = (fg_rgb >> 8) & 0xFFu;
    uint32_t fg_b = fg_rgb & 0xFFu;

    uint32_t out_r = (fg_r * alpha + bg_r * inv + 127u) / 255u;
    uint32_t out_g = (fg_g * alpha + bg_g * inv + 127u) / 255u;
    uint32_t out_b = (fg_b * alpha + bg_b * inv + 127u) / 255u;

    return 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
}

static void font_blit_glyph_scaled_internal(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                            uint32_t x, uint32_t y, char ch,
                                            uint32_t fg_color, float scale) {
    glyph_cache_entry_t *entry = font_get_cached_glyph(ch, fg_color, scale);
    if (!entry || entry->scaled_w == 0 || entry->scaled_h == 0 || fb_pitch < 4) {
        return;
    }

    uint32_t stride = fb_pitch / 4;
    uint32_t fg_rgb = entry->fg_color;
    uint32_t fg_alpha = entry->fg_alpha;
    for (uint32_t row = 0; row < entry->scaled_h; row++) {
        uint32_t dst_y = y + row;
        if (dst_y >= fb_height) {
            break;
        }

        uint32_t start = entry->row_start[row];
        uint32_t end = entry->row_end[row];
        if (end <= start) {
            continue;
        }

        for (uint32_t col = start; col < end; col++) {
            uint32_t dst_x = x + col;
            if (dst_x >= fb_width) {
                break;
            }
            uint8_t alpha = entry->alpha_map[(size_t)row * entry->scaled_w + col];
            if (alpha == 0) {
                continue;
            }
            alpha = (uint8_t)(((uint32_t)alpha * fg_alpha + 127u) / 255u);

            size_t idx = (size_t)dst_y * stride + dst_x;
            uint32_t *dst_px = &((uint32_t *)fb)[idx];
            *dst_px = blend_over_rgb(*dst_px, fg_rgb, alpha);
        }
    }
}

static void font_blit_glyph_scaled_internal_rot90ccw(uint8_t *fb,
                                                     uint32_t fb_width,
                                                     uint32_t fb_height,
                                                     uint32_t fb_pitch,
                                                     uint32_t logical_width,
                                                     uint32_t logical_height,
                                                     uint32_t x,
                                                     uint32_t y,
                                                     char ch,
                                                     uint32_t fg_color,
                                                     float scale) {
    glyph_cache_entry_t *entry = font_get_cached_glyph(ch, fg_color, scale);
    if (!entry || entry->scaled_w == 0 || entry->scaled_h == 0 ||
        fb_pitch < 4 || logical_width == 0 || logical_height == 0) {
        return;
    }

    uint32_t stride = fb_pitch / 4;
    uint32_t fg_rgb = entry->fg_color;
    uint32_t fg_alpha = entry->fg_alpha;

    for (uint32_t row = 0; row < entry->scaled_h; row++) {
        uint32_t logical_y = y + row;
        if (logical_y >= logical_height) {
            break;
        }

        uint32_t start = entry->row_start[row];
        uint32_t end = entry->row_end[row];
        if (end <= start) {
            continue;
        }

        for (uint32_t col = start; col < end; col++) {
            uint32_t logical_x = x + col;
            if (logical_x >= logical_width) {
                break;
            }

            uint8_t alpha = entry->alpha_map[(size_t)row * entry->scaled_w + col];
            if (alpha == 0) {
                continue;
            }
            alpha = (uint8_t)(((uint32_t)alpha * fg_alpha + 127u) / 255u);

            uint32_t phys_x = logical_y;
            uint32_t phys_y = logical_width - 1 - logical_x;
            if (phys_x >= fb_width || phys_y >= fb_height) {
                continue;
            }

            uint32_t pixel_offset = (phys_y * stride) + phys_x;
            uint32_t *dst_px = &((uint32_t *)fb)[pixel_offset];
            *dst_px = blend_over_rgb(*dst_px, fg_rgb, alpha);
        }
    }
}

void font_blit_glyph(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                     uint32_t x, uint32_t y, char ch,
                     uint32_t fg_color) {
    font_blit_glyph_scaled_internal(fb, fb_width, fb_height, fb_pitch, x, y, ch, fg_color, 1.0f);
}

void font_blit_string(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                      uint32_t x, uint32_t y, const char *text,
                      uint32_t fg_color) {
    font_blit_string_scaled(fb, fb_width, fb_height, fb_pitch, x, y, text, fg_color, 1.0f);
}

void font_blit_string_scaled(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                             uint32_t x, uint32_t y, const char *text,
                             uint32_t fg_color, float scale) {
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    uint32_t cur_x = x;
    uint32_t prev_x = x;

    for (const char *p = text; *p; p++) {
        char ch = *p;

        /* Find glyph */
        glyph_t *glyph = font_find_glyph(ch);

        if (!glyph || !glyph->pixels) {
            continue;
        }

        uint32_t scaled_width = (uint32_t)(glyph->width * scale + 0.5f);
        uint32_t scaled_spacing = (uint32_t)(glyph->spacing_width * scale + 0.5f);
        if (scaled_spacing == 0 && glyph->spacing_width > 0) {
            scaled_spacing = 1;
        }
        uint32_t period_lift = (uint32_t)(scale + 0.5f);

        /* Period overlays at same x as previous character, 1px up */
        uint32_t draw_x = cur_x;
        uint32_t draw_y = y;
        if (ch == '.') {
            draw_x = prev_x;
            draw_y = (y > period_lift) ? (y - period_lift) : y;
        }

        if (draw_x + scaled_width > fb_width) {
            break;
        }

        font_blit_glyph_scaled_internal(fb, fb_width, fb_height, fb_pitch,
                                        draw_x, draw_y, ch,
                                        fg_color, scale);

        /* Track previous x before advancing */
        if (ch != '.') {
            prev_x = cur_x;
        }

        /* Advance cursor by spacing_width */
        cur_x += scaled_spacing;
    }
}

void font_blit_string_scaled_rot90ccw(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                      uint32_t logical_width, uint32_t logical_height,
                                      uint32_t x, uint32_t y, const char *text,
                                      uint32_t fg_color, float scale) {
    if (scale <= 0.0f) {
        scale = 1.0f;
    }

    uint32_t cur_x = x;
    uint32_t prev_x = x;

    for (const char *p = text; *p; p++) {
        char ch = *p;

        glyph_t *glyph = font_find_glyph(ch);
        if (!glyph || !glyph->pixels) {
            continue;
        }

        uint32_t scaled_width = (uint32_t)(glyph->width * scale + 0.5f);
        uint32_t scaled_spacing = (uint32_t)(glyph->spacing_width * scale + 0.5f);
        if (scaled_spacing == 0 && glyph->spacing_width > 0) {
            scaled_spacing = 1;
        }
        uint32_t period_lift = (uint32_t)(scale + 0.5f);

        uint32_t draw_x = cur_x;
        uint32_t draw_y = y;
        if (ch == '.') {
            draw_x = prev_x;
            draw_y = (y > period_lift) ? (y - period_lift) : y;
        }

        if (draw_x + scaled_width > logical_width) {
            break;
        }

        font_blit_glyph_scaled_internal_rot90ccw(fb, fb_width, fb_height, fb_pitch,
                                                 logical_width, logical_height,
                                                 draw_x, draw_y, ch,
                                                 fg_color, scale);

        if (ch != '.') {
            prev_x = cur_x;
        }

        cur_x += scaled_spacing;
    }
}

void font_blit_background_scaled_rot90ccw(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                          uint32_t logical_width, uint32_t logical_height,
                                          uint32_t x, uint32_t y, float scale) {
    uint32_t glyph_color = 0xFF0A0A0A;
    const char *bg_pattern = "8.8.8.8.8.8.8.8.";

    font_blit_string_scaled_rot90ccw(fb, fb_width, fb_height, fb_pitch,
                                     logical_width, logical_height,
                                     x, y, bg_pattern,
                                     glyph_color, scale);
}

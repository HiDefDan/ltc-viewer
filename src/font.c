#include "font.h"
#include "font-render.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* CPU-fallback 7-segment renderer. Glyph bitmaps come from the shared
   font-render module (rasterized once from the vendored DSEG7 TTF at
   FONT_GLYPH_HEIGHT), already gamma-corrected — the coverage value IS the
   blend alpha. The period cell has zero advance and overlays the previous
   digit, same as the GPU path. */

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

static void free_cache_entry(glyph_cache_entry_t *entry) {
    if (!entry) {
        return;
    }
    free(entry->alpha_map);
    free(entry->row_start);
    free(entry->row_end);
    memset(entry, 0, sizeof(*entry));
}

int font_init(void) {
    fflush(stdout);

    for (size_t i = 0; i < (sizeof(glyph_cache) / sizeof(glyph_cache[0])); i++) {
        free_cache_entry(&glyph_cache[i]);
    }
    for (int i = 0; i < 11; i++) {
        free(glyph_map[i].pixels);
        memset(&glyph_map[i], 0, sizeof(glyph_map[i]));
    }

    fr_glyph_set_t set;
    if (font_render_rasterize_set(&set, FONT_GLYPH_HEIGHT) != 0) {
        fprintf(stderr, "[FONT] Font rasterization failed\n");
        return -1;
    }

    /* Adopt the rasterized cell bitmaps directly (coverage == blend alpha,
       gamma already applied by the rasterizer). */
    const char *glyph_chars = "0123456789.";
    for (int i = 0; i < 11; i++) {
        glyph_map[i].ch = glyph_chars[i];
        glyph_map[i].width = set.glyphs[i].cell_w;
        glyph_map[i].height = set.glyphs[i].cell_h;
        glyph_map[i].spacing_width = set.glyphs[i].advance;
        glyph_map[i].pixels = set.glyphs[i].bitmap;
        glyph_map[i].pixels_size = (size_t)set.glyphs[i].cell_w * set.glyphs[i].cell_h;
        set.glyphs[i].bitmap = NULL; /* ownership moved to glyph_map */
    }

    printf("[FONT] Glyphs rasterized at %d px (digit advance %u px)\n",
           FONT_GLYPH_HEIGHT, glyph_map[0].spacing_width);
    fflush(stdout);
    return 0;
}

uint32_t font_digit_advance(void) {
    return glyph_map[0].spacing_width ? glyph_map[0].spacing_width : 1;
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
    /* Render "88.88.88.88." - subtle dark gray visible on black canvas */
    uint32_t glyph_color = 0xFF0A0A0A;   /* RGB(10, 10, 10) - dark plastic, subtle but visible */
    const char *bg_pattern = "88.88.88.88.";

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
    uint32_t fg_rgb_key = fg_color & 0x00FFFFFFu;
    uint8_t fg_alpha_key = (uint8_t)((fg_color >> 24) & 0xFFu);
    if (fg_alpha_key == 0) {
        fg_alpha_key = 0xFFu;
    }
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

        if (entry->ch == ch &&
            entry->fg_color == fg_rgb_key &&
            entry->fg_alpha == fg_alpha_key &&
            entry->scale_key == scale_key) {
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

            /* Coverage is already gamma-corrected by the rasterizer. */
            uint8_t gamma_alpha = glyph->pixels[src_row * glyph->width + src_col];
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
    entry->fg_color = fg_rgb_key;
    entry->fg_alpha = fg_alpha_key;
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

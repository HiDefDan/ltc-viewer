#include "font-render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define FONT_FILE_NAME "DSEG7Classic-Bold.ttf"
#define FONT_INSTALLED_DIR "/usr/share/ltc-timecode/fonts"
#define FONT_SOURCE_DIR "data/fonts"

/* Coverage-to-alpha response, shared by the GL and CPU consumers. The CPU
   blitter historically applied this at blit time; it now lives here so both
   paths get identical glyph weight. */
#define FONT_ALPHA_GAMMA 0.90f

static unsigned char *g_font_data = NULL;
static stbtt_fontinfo g_font;
static int g_font_ready = 0;
static int g_digit_advance_units = 0;   /* uniform digit advance, font units */
static float g_unit_scale_1px = 0.0f;   /* stbtt scale for 1px height */
static uint8_t g_gamma_lut[256];

static int load_font_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    if (size <= 0) { fclose(fp); return -1; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    unsigned char *data = malloc((size_t)size);
    if (!data) { fclose(fp); return -1; }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    g_font_data = data;
    return 0;
}

int font_render_init(void) {
    if (g_font_ready) return 0;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", FONT_INSTALLED_DIR, FONT_FILE_NAME);
    if (load_font_file(path) != 0) {
        snprintf(path, sizeof(path), "%s/%s", FONT_SOURCE_DIR, FONT_FILE_NAME);
        if (load_font_file(path) != 0) {
            fprintf(stderr, "[FONT-RENDER] Failed to read font file %s (also tried %s/%s)\n",
                    path, FONT_INSTALLED_DIR, FONT_FILE_NAME);
            return -1;
        }
    }

    if (!stbtt_InitFont(&g_font, g_font_data, stbtt_GetFontOffsetForIndex(g_font_data, 0))) {
        fprintf(stderr, "[FONT-RENDER] stbtt_InitFont failed for %s\n", path);
        free(g_font_data);
        g_font_data = NULL;
        return -1;
    }

    /* Uniform monospace digit advance: take the max defensively. */
    for (int d = 0; d < 10; d++) {
        int glyph = stbtt_FindGlyphIndex(&g_font, '0' + d);
        if (glyph == 0) {
            fprintf(stderr, "[FONT-RENDER] Font is missing digit glyph '%c'\n", '0' + d);
            free(g_font_data);
            g_font_data = NULL;
            return -1;
        }
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&g_font, glyph, &adv, &lsb);
        if (adv > g_digit_advance_units) g_digit_advance_units = adv;
    }
    if (stbtt_FindGlyphIndex(&g_font, '.') == 0) {
        fprintf(stderr, "[FONT-RENDER] Font is missing the period glyph\n");
        free(g_font_data);
        g_font_data = NULL;
        return -1;
    }

    g_unit_scale_1px = stbtt_ScaleForPixelHeight(&g_font, 1.0f);

    for (int i = 0; i < 256; i++) {
        float n = (float)i / 255.0f;
        int v = (int)lroundf(powf(n, FONT_ALPHA_GAMMA) * 255.0f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        g_gamma_lut[i] = (uint8_t)v;
    }

    g_font_ready = 1;
    fprintf(stderr, "[FONT-RENDER] Loaded %s (digit aspect %.3f)\n",
            path, font_render_digit_aspect());
    return 0;
}

float font_render_digit_aspect(void) {
    if (!g_font_ready && font_render_init() != 0) return 0.0f;
    return (float)g_digit_advance_units * g_unit_scale_1px;
}

/* Rasterize one glyph and compose it into a zero-filled cell with the
   pen at (pen_x, baseline == cell_h), applying the gamma LUT. */
static int compose_glyph(fr_glyph_t *out, int glyph, float scale,
                         uint32_t cell_w, uint32_t cell_h, int32_t pen_x) {
    out->cell_w = cell_w;
    out->cell_h = cell_h;
    out->bitmap = calloc((size_t)cell_w * cell_h, 1);
    if (!out->bitmap) return -1;

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&g_font, glyph, scale, scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0;
    int gh = y1 - y0;
    if (gw <= 0 || gh <= 0) return 0;   /* empty glyph: blank cell */

    unsigned char *tmp = malloc((size_t)gw * gh);
    if (!tmp) { free(out->bitmap); out->bitmap = NULL; return -1; }
    stbtt_MakeGlyphBitmap(&g_font, tmp, gw, gh, gw, scale, scale, glyph);

    /* Baseline sits at the cell bottom (DSEG7: ascent == em, descent == 0),
       so glyph rows map to cell rows [cell_h + y0, cell_h + y1). */
    for (int row = 0; row < gh; row++) {
        int32_t cy = (int32_t)cell_h + y0 + row;
        if (cy < 0 || cy >= (int32_t)cell_h) continue;
        for (int col = 0; col < gw; col++) {
            int32_t cx = pen_x + x0 + col;
            if (cx < 0 || cx >= (int32_t)cell_w) continue;
            out->bitmap[(size_t)cy * cell_w + cx] = g_gamma_lut[tmp[(size_t)row * gw + col]];
        }
    }
    free(tmp);
    return 0;
}

int font_render_rasterize_set(fr_glyph_set_t *set, uint32_t pixel_height) {
    if (!set || pixel_height == 0) return -1;
    if (font_render_init() != 0) return -1;

    memset(set, 0, sizeof(*set));
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)pixel_height);
    uint32_t digit_cell_w = (uint32_t)lroundf((float)g_digit_advance_units * scale);
    if (digit_cell_w == 0) digit_cell_w = 1;
    set->digit_cell_w = digit_cell_w;
    set->cell_h = pixel_height;

    for (int d = 0; d < 10; d++) {
        int glyph = stbtt_FindGlyphIndex(&g_font, '0' + d);
        if (compose_glyph(&set->glyphs[d], glyph, scale,
                          digit_cell_w, pixel_height, 0) != 0) {
            font_render_free_set(set);
            return -1;
        }
        set->glyphs[d].advance = digit_cell_w;
    }

    /* Period: pen at the previous digit's right edge (digit_cell_w). DSEG7's
       '.' has zero native advance and a bitmap box straddling the pen, so
       the dot lands centered on the digit boundary — same overlay the old
       period.png encoded. Cell extends to cover the dot's right half. */
    int pg = stbtt_FindGlyphIndex(&g_font, '.');
    int px0, py0, px1, py1;
    stbtt_GetGlyphBitmapBox(&g_font, pg, scale, scale, &px0, &py0, &px1, &py1);
    uint32_t period_cell_w = digit_cell_w + (px1 > 0 ? (uint32_t)px1 : 0);
    if (compose_glyph(&set->glyphs[FR_PERIOD_INDEX], pg, scale,
                      period_cell_w, pixel_height, (int32_t)digit_cell_w) != 0) {
        font_render_free_set(set);
        return -1;
    }
    set->glyphs[FR_PERIOD_INDEX].advance = 0;

    /* 'd'/'F' for the rate-family indicator ("dF" — 29.97 drop-frame vs 30
       non-drop). Composed the same way as digits: monospace cell, pen at
       cell origin, advance = digit_cell_w. Any letter wider than a digit
       cell simply clips at the cell edge (compose_glyph's existing bounds
       check) rather than corrupting adjacent cells. */
    int dg = stbtt_FindGlyphIndex(&g_font, 'd');
    if (compose_glyph(&set->glyphs[FR_D_INDEX], dg, scale,
                      digit_cell_w, pixel_height, 0) != 0) {
        font_render_free_set(set);
        return -1;
    }
    set->glyphs[FR_D_INDEX].advance = digit_cell_w;

    int fg = stbtt_FindGlyphIndex(&g_font, 'F');
    if (compose_glyph(&set->glyphs[FR_F_INDEX], fg, scale,
                      digit_cell_w, pixel_height, 0) != 0) {
        font_render_free_set(set);
        return -1;
    }
    set->glyphs[FR_F_INDEX].advance = digit_cell_w;

    return 0;
}

void font_render_free_set(fr_glyph_set_t *set) {
    if (!set) return;
    for (int i = 0; i < FR_GLYPH_COUNT; i++) {
        free(set->glyphs[i].bitmap);
        set->glyphs[i].bitmap = NULL;
    }
}

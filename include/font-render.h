#ifndef FONT_RENDER_H
#define FONT_RENDER_H

#include <stdint.h>

/* Direct font rasterizer: renders the timecode glyph set (digits 0-9 and
   the period) from the vendored DSEG7 TTF at an arbitrary pixel height.
   Startup-only — never called from the render hot path.

   Cell model (matches the retired pre-rendered PNG assets):
   - Digits are uniform monospace cells of digit_cell_w x cell_h with the
     glyph baseline at the cell bottom (DSEG7 has ascent=em, descent=0).
   - The period is a wide cell (digit_cell_w plus the dot's right
     extent) with advance 0: drawn at the previous digit's x, the dot
     lands centered on that digit's right edge and the cursor does not
     move. This zero-advance overlay is load-bearing for the string
     centering math in main.c. */

#define FR_GLYPH_COUNT 13   /* [0..9] = digits, [10] = period, [11] = 'd', [12] = 'F' */
#define FR_PERIOD_INDEX 10
#define FR_D_INDEX 11       /* lowercase 'd', for the "dF" rate-family indicator */
#define FR_F_INDEX 12       /* uppercase 'F' */

typedef struct {
    uint32_t cell_w;    /* bitmap width in px */
    uint32_t cell_h;    /* bitmap height in px (== requested pixel height) */
    uint32_t advance;   /* cursor advance in px; 0 for the period */
    uint8_t *bitmap;    /* cell_w * cell_h coverage values, gamma-corrected */
} fr_glyph_t;

typedef struct {
    fr_glyph_t glyphs[FR_GLYPH_COUNT];
    uint32_t digit_cell_w;  /* uniform digit cell width (monospace advance) */
    uint32_t cell_h;        /* uniform cell height */
} fr_glyph_set_t;

/* Load the TTF into memory and parse it. Idempotent; returns 0 on success.
   Looks in /usr/share/ltc-timecode/fonts first, then data/fonts. */
int font_render_init(void);

/* Digit advance divided by pixel height (DSEG7 Classic: ~0.816).
   Valid after font_render_init(); returns 0.0f if not initialized. */
float font_render_digit_aspect(void);

/* Rasterize all 11 glyphs at the given pixel height. Returns 0 on success.
   The set owns heap bitmaps; release with font_render_free_set(). */
int font_render_rasterize_set(fr_glyph_set_t *set, uint32_t pixel_height);

void font_render_free_set(fr_glyph_set_t *set);

#endif /* FONT_RENDER_H */

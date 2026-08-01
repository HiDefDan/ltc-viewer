#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>

/* Pixel height the CPU-fallback glyphs are rasterized at (the scaled-glyph
   LRU cache rescales from this base at blit time). */
#define FONT_GLYPH_HEIGHT 256

/* CPU-fallback 7-segment font renderer:
   Rasterizes 11 glyphs (digits 0-9 and period) from the vendored DSEG7 TTF
   via the font-render module and blits them to an ARGB8888 framebuffer.

   Format:
   - System time fallback: HH.MM.SS (3 periods separating hours, minutes, seconds)
   - LTC timecode input: HH.MM.SS.FF (3 periods, with frame numbers 0-30 at 30Hz) */

/* Initialize font: rasterizes the 11-glyph set at FONT_GLYPH_HEIGHT */
int font_init(void);

/* Digit cell advance in px at the base FONT_GLYPH_HEIGHT rasterization
   (valid after font_init; never 0). */
uint32_t font_digit_advance(void);

/* Prepare background layer (glyph-based unlit 7-segment grid) */
void font_load_background(void);

/* Render background layer to ARGB8888 framebuffer at (x, y).
   Renders "8.8.8.8.8.8.8.8." pattern in gray (88,88,88) to show all segment positions with periods. */
void font_blit_background(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                          uint32_t x, uint32_t y);

/* Render background layer with uniform glyph scale */
void font_blit_background_scaled(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                 uint32_t x, uint32_t y, float scale);

/* Render background layer while mapping from a logical landscape canvas
   into a physical portrait framebuffer (90 deg counter-clockwise). */
void font_blit_background_scaled_rot90ccw(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                          uint32_t logical_width, uint32_t logical_height,
                                          uint32_t x, uint32_t y, float scale);

/* Render a single glyph to ARGB8888 framebuffer at (x, y).
   Only bright pixels (> 128) are written; dark pixels are skipped. */
void font_blit_glyph(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                     uint32_t x, uint32_t y, char ch,
                     uint32_t fg_color);

/* Render a NULL-terminated string. Period (.) overlays at the previous character's position
   for proper timecode formatting (HH.MM.SS). */
void font_blit_string(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                      uint32_t x, uint32_t y, const char *text,
                      uint32_t fg_color);

/* Render string with uniform glyph scale */
void font_blit_string_scaled(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                             uint32_t x, uint32_t y, const char *text,
                             uint32_t fg_color, float scale);

/* Render string while mapping from a logical landscape canvas
   into a physical portrait framebuffer (90 deg counter-clockwise). */
void font_blit_string_scaled_rot90ccw(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                                      uint32_t logical_width, uint32_t logical_height,
                                      uint32_t x, uint32_t y, const char *text,
                                      uint32_t fg_color, float scale);

#endif /* FONT_H */

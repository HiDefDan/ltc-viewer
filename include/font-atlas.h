#ifndef FONT_ATLAS_H
#define FONT_ATLAS_H

#include <stdint.h>

#define FONT_ATLAS_WIDTH 2304
#define FONT_ATLAS_HEIGHT 257
#define FONT_ATLAS_GLYPH_COUNT 11

extern const uint32_t font_atlas_glyph_widths[FONT_ATLAS_GLYPH_COUNT];
extern const uint32_t font_atlas_glyph_heights[FONT_ATLAS_GLYPH_COUNT];
extern const uint32_t font_atlas_glyph_spacing[FONT_ATLAS_GLYPH_COUNT];
extern const float font_atlas_glyph_uv[FONT_ATLAS_GLYPH_COUNT][4];
extern const uint8_t font_atlas_pixels[FONT_ATLAS_WIDTH * FONT_ATLAS_HEIGHT * 4];

#endif // FONT_ATLAS_H

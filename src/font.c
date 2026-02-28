#include "font.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Individual glyph storage */
typedef struct {
    char ch;
    uint32_t width;
    uint32_t height;
    uint32_t spacing_width;  /* How many pixels to advance cursor */
    uint8_t *pixels;         /* Grayscale pixel data */
    size_t pixels_size;
} glyph_t;

static glyph_t glyph_map[11] = {0};  /* 0-9 + period */

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
    /* Render "8.8.8.8.8.8.8.8." - subtle dark gray visible on black canvas */
    uint32_t glyph_color = 0xFF0A0A0A;  /* RGB(10, 10, 10) - dark gray, subtle but visible */
    const char *bg_pattern = "8.8.8.8.8.8.8.8.";
    
    font_blit_string(fb, fb_width, fb_height, fb_pitch,
                     x, y, bg_pattern,
                     glyph_color);
}

void font_blit_glyph(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                     uint32_t x, uint32_t y, char ch,
                     uint32_t fg_color) {
    /* Find glyph */
    glyph_t *glyph = NULL;
    for (int i = 0; i < 11; i++) {
        if (glyph_map[i].ch == ch) {
            glyph = &glyph_map[i];
            break;
        }
    }

    if (!glyph || !glyph->pixels) {
        return;
    }

    uint32_t glyph_w = glyph->width;
    uint32_t glyph_h = glyph->height;

    /* Blit glyph from individual PNG (grayscale) to ARGB8888 framebuffer */
    for (uint32_t row = 0; row < glyph_h && (y + row) < fb_height; row++) {
        for (uint32_t col = 0; col < glyph_w && (x + col) < fb_width; col++) {
            uint8_t gray_val = glyph->pixels[row * glyph->width + col];

            /* Only render bright pixels (> 128); skip dark pixels */
            if (gray_val > 128) {
                uint32_t pixel_offset = ((y + row) * (fb_pitch / 4)) + (x + col);
                uint32_t *pixel_ptr = (uint32_t *)&fb[pixel_offset * 4];
                *pixel_ptr = fg_color;
            }
        }
    }
}

void font_blit_string(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                      uint32_t x, uint32_t y, const char *text,
                      uint32_t fg_color) {
    uint32_t cur_x = x;
    uint32_t prev_x = x;

    for (const char *p = text; *p; p++) {
        char ch = *p;

        /* Find glyph */
        glyph_t *glyph = NULL;
        for (int i = 0; i < 11; i++) {
            if (glyph_map[i].ch == ch) {
                glyph = &glyph_map[i];
                break;
            }
        }

        if (!glyph || !glyph->pixels) {
            continue;
        }

        /* Period overlays at same x as previous character, 1px up */
        uint32_t draw_x = cur_x;
        uint32_t draw_y = y;
        if (ch == '.') {
            draw_x = prev_x;
            draw_y = (y > 0) ? (y - 1) : y;
        }

        if (draw_x + glyph->width > fb_width) {
            break;
        }

        font_blit_glyph(fb, fb_width, fb_height, fb_pitch,
                       draw_x, draw_y, ch,
                       fg_color);

        /* Track previous x before advancing */
        if (ch != '.') {
            prev_x = cur_x;
        }

        /* Advance cursor by spacing_width */
        cur_x += glyph->spacing_width;
    }
}

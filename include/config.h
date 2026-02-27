#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Hardware configuration */
#define GPIO_LTC_PIN            17              /* BCM GPIO 17 for LTC input */
#define TARGET_REFRESH_HZ       50              /* 50 Hz for 25fps LTC */
#define DISPLAY_WIDTH           1920            /* 1920x1080 @ 50Hz */
#define DISPLAY_HEIGHT          1080

/* Display layout */
#define TIMECODE_X              333
#define TIMECODE_Y              412
#define DISPLAY_DIGIT_WIDTH     209             /* Width of each digit glyph */
#define BACKGROUND_X            (TIMECODE_X - DISPLAY_DIGIT_WIDTH)  /* Background starts 1 digit before timecode */
#define TIMECODE_FONT_HEIGHT    300             /* Large, visible glyphs for 1920x1080 */

/* Colors (ARGB8888) */
#define COLOR_BLACK             0xFF000000
#define COLOR_WHITE             0xFF40FF40
#define COLOR_RED               0xFFFF0000
#define COLOR_GREEN             0xFF00FF00
#define COLOR_CYAN              0xFF00FFFF

/* Timezone for display (e.g., "Europe/London") */
#define TZ_STRING               "Europe/London"

/* Whether to display system time or pure LTC */
#define SHOW_SYSTEM_TIME        1

/* LTC frame rate (24/25/30 fps) */
#define LTC_FRAME_RATE          25

#endif /* CONFIG_H */

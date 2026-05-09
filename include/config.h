#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Hardware configuration */
#define GPIO_LTC_PIN            17              /* BCM GPIO 17 for LTC input */
#define TARGET_REFRESH_HZ       60              /* Waveshare 8.8" DSI panel max 60 Hz */
#define DISPLAY_WIDTH           1920            /* Landscape after cmdline rotate=90 */
#define DISPLAY_HEIGHT          480             /* Waveshare 8.8" DSI: 480x1920 native, rotated 90° */

/* Display layout */
#define TIMECODE_X              80
#define TIMECODE_Y              90
#define DISPLAY_DIGIT_WIDTH     209             /* Match actual digit spacing width from font renderer */
#define BACKGROUND_X            (TIMECODE_X - DISPLAY_DIGIT_WIDTH)  /* Background starts 1 digit before timecode */
#define TIMECODE_FONT_HEIGHT    288             /* Target glyph height at 1920x480 baseline */

/* Colors (ARGB8888) */
#define COLOR_BLACK             0xFF000000
#define COLOR_WHITE             0xFF40FF40
#define COLOR_RED               0xFFFF0000
#define COLOR_GREEN             0xFF00FF00
#define COLOR_CYAN              0xFF00FFFF

/* Timezone for display (e.g., "Europe/London") */
#define TZ_STRING               "Europe/London"

/* Whether to display system time or pure LTC */
#define SHOW_SYSTEM_TIME        0

/* LTC frame rate (24/25/30 fps) */
#define LTC_FRAME_RATE          25

#endif /* CONFIG_H */

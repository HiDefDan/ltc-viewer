#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Set from LTC_LOG_VERBOSE at startup (main.c). When 0 (default), the
 * render loop and LTC decoder only log transitions (lock/unlock, gaps,
 * layout changes) — not one line per rendered frame or decoded LTC frame,
 * which at 25-60/s otherwise floods the journal. Set LTC_LOG_VERBOSE=1 to
 * restore full per-frame logging for deep debugging. */
extern int g_log_verbose;

/* Hardware configuration */
#define TARGET_REFRESH_HZ       60              /* Waveshare 8.8" DSI panel max 60 Hz */
#define DISPLAY_WIDTH           1920            /* Landscape after cmdline rotate=90 */
#define DISPLAY_HEIGHT          480             /* Waveshare 8.8" DSI: 480x1920 native, rotated 90° */

/* Display layout */
#define TIMECODE_X              80
#define TIMECODE_Y              90
#define TIMECODE_FONT_HEIGHT    288             /* Target glyph pixel height (clamped per output mode) */

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
#define LTC_FRAME_RATE          30

/* LTC loss behavior */
#define LTC_LOSS_HOLD_LAST_FRAME    0
#define LTC_LOSS_RESTORE_TOD        1

/* ALSA capture rate (used by libltc APV calculation) */
#define ALSA_CAPTURE_RATE           48000

/* RtAudio capture buffer size in frames (period).
 * RtAudio+ALSA will round to nearest hardware-supported value.
 * 64 frames @ 48kHz = 1.33ms per callback — well below LTC bit period. */
#define RTAUDIO_CAPTURE_PERIOD_FRAMES 64

#endif /* CONFIG_H */

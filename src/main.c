#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/mman.h>
#include <math.h>

#include "drm.h"
#include "font.h"
#include "ltc.h"
#include "gpio.h"
#include "config.h"
#include "config-management.h"
#include "config-watcher.h"

static volatile int should_exit = 0;

void signal_handler(int sig) {
    (void)sig;
    should_exit = 1;
}

int main(int argc, char *argv[]) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] Started at boot+%.3f seconds\n", ts.tv_sec + ts.tv_nsec/1e9);

    (void)argc;
    (void)argv;

    printf("LTC Viewer v1.0 (Raspberry Pi 5)\n");

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Enable real-time scheduling (SCHED_FIFO priority 50) */
    struct sched_param param = {0};
    param.sched_priority = 50;
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        fprintf(stderr, "[MAIN] Warning: Failed to set real-time scheduling (requires CAP_SYS_NICE)\n");
        perror("[MAIN] sched_setscheduler");
    } else {
        printf("[MAIN] Real-time scheduling enabled (SCHED_FIFO priority 50)\n");
    }

    /* Lock memory to prevent page faults */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[MAIN] Warning: Failed to lock memory\n");
        perror("[MAIN] mlockall");
    } else {
        printf("[MAIN] Memory locked\n");
    }

    /* Initialize config watcher early */
    config_watcher_t config_watcher = {0};
    const char *config_path = "/etc/ltc-viewer/config.json";
    if (config_watcher_init(&config_watcher, config_path) != 0) {
        fprintf(stderr, "Warning: Failed to initialize config watcher\n");
    } else {
        printf("[MAIN] Config watcher initialized for %s\n", config_path);
    }

    /* Load initial configuration BEFORE DRM init */
    ltc_config_t current_config = {0};
    config_load(config_path, &current_config);
        printf("[MAIN] Initial config loaded: TZ=%s, NTP=%s, Mode=%dx%d@%.2f\n",
            current_config.timezone, current_config.ntp_server,
            current_config.display_width, current_config.display_height, current_config.refresh_hz);

    /* Initialize DRM for framebuffer rendering with config refresh rate */
    ltc_drm_context_t drm = {0};
    float target_hz = (current_config.refresh_hz > 0.0f) ? current_config.refresh_hz : (float)TARGET_REFRESH_HZ;
    if (drm_init(&drm, (uint32_t)current_config.display_width, (uint32_t)current_config.display_height, target_hz)) {
        fprintf(stderr, "Failed to initialize DRM\n");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] DRM initialized: %.2f Hz (boot+%.3f s)\n", drm.mode_refresh_hz, ts.tv_sec + ts.tv_nsec/1e9);

    /* Update current_config to reflect what DRM actually selected */
    current_config.display_width = drm.front.hdisplay;
    current_config.display_height = drm.front.vdisplay;
    current_config.refresh_hz = drm.mode_refresh_hz;

    /* Initialize bitmap font */
    if (font_init()) {
        fprintf(stderr, "Failed to initialize font\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] Font initialized (boot+%.3f s)\n", ts.tv_sec + ts.tv_nsec/1e9);
    fflush(stdout);

    /* Load background layer (unlit 7-segment grid) */
    font_load_background();

    /* Initialize LTC decoder */
    ltc_decoder_t ltc = {0};
    if (ltc_decoder_init(&ltc, LTC_FRAME_RATE)) {
        fprintf(stderr, "Failed to initialize LTC decoder\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] LTC decoder initialized for %u fps (boot+%.3f s)\n", LTC_FRAME_RATE, ts.tv_sec + ts.tv_nsec/1e9);
    fflush(stdout);

    /* Initialize GPIO for LTC edge capture */
    gpio_context_t gpio = {0};
    if (gpio_init(&gpio, GPIO_LTC_PIN)) {
        fprintf(stderr, "Failed to initialize GPIO\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    fflush(stdout);
    printf("[MAIN] GPIO initialized on BCM%d (boot+%.3f s)\n", GPIO_LTC_PIN, ts.tv_sec + ts.tv_nsec/1e9);

    /* Main render loop */
    uint64_t frame_count = 0;
    time_t last_time_display = 0;

    clock_gettime(CLOCK_BOOTTIME, &ts);
    fflush(stdout);
    printf("[MAIN] Starting render loop at boot+%.3f s\n", ts.tv_sec + ts.tv_nsec/1e9);

    while (!should_exit) {
        /* Check if config file has changed */
        if (config_watcher_check(&config_watcher) > 0) {
            printf("[MAIN] Config file changed, reloading...\n");
            fflush(stdout);
            ltc_config_t new_config = {0};
            if (config_load(config_path, &new_config) == 0) {
                  /* Check if mode changed */
                  int refresh_changed = (fabsf(new_config.refresh_hz - current_config.refresh_hz) > 0.01f && new_config.refresh_hz > 0);
                  int resolution_changed = (new_config.display_width != current_config.display_width ||
                                new_config.display_height != current_config.display_height);

                  if (refresh_changed || resolution_changed) {
                      printf("[MAIN] Display mode changed from %dx%d@%.2f to %dx%d@%.2f, reinitializing DRM...\n",
                          current_config.display_width, current_config.display_height, current_config.refresh_hz,
                          new_config.display_width, new_config.display_height, new_config.refresh_hz);
                    fflush(stdout);
                    drm_cleanup(&drm);
                    float new_target_hz = new_config.refresh_hz;
                      if (drm_init(&drm, (uint32_t)new_config.display_width, (uint32_t)new_config.display_height, new_target_hz) == 0) {
                       printf("[MAIN] DRM reinitialized successfully: %ux%u @ %.2f Hz\n",
                           drm.front.hdisplay, drm.front.vdisplay, drm.mode_refresh_hz);
                        fflush(stdout);
                        /* Update new_config to reflect what DRM actually selected */
                        new_config.display_width = drm.front.hdisplay;
                        new_config.display_height = drm.front.vdisplay;
                        new_config.refresh_hz = drm.mode_refresh_hz;
                    } else {
                       fprintf(stderr, "[MAIN] Failed to reinitialize DRM with %dx%d@%.2f, exiting\n",
                            new_config.display_width, new_config.display_height, new_config.refresh_hz);
                        fflush(stderr);
                        should_exit = 1;
                    }
                }
                current_config = new_config;
                  printf("[MAIN] Config reloaded: TZ=%s, NTP=%s, Mode=%dx%d@%.2f, Pos=(+%d,+%d), Color=(%d,%d,%d)\n",
                      current_config.timezone, current_config.ntp_server,
                      current_config.display_width, current_config.display_height, current_config.refresh_hz,
                       current_config.timecode_x_offset, current_config.timecode_y_offset,
                       current_config.color_r, current_config.color_g, current_config.color_b);
                fflush(stdout);
            }
        }

        /* Poll GPIO for edge (non-blocking stub) */
        uint64_t edge_time = 0;
        int level = 0;
        if (gpio_wait_edge(&gpio, &edge_time, &level) == 0 && edge_time > 0) {
            ltc_feed_edge(&ltc, edge_time, level);
        }

        /* Get back buffer and render */
        uint8_t *back_buffer = drm_get_back_buffer(&drm);
        if (!back_buffer) {
            fprintf(stderr, "Failed to get back buffer\n");
            break;
        }

        uint32_t render_width = drm.back.hdisplay;
        uint32_t render_height = drm.back.vdisplay;

        float scale_x = (float)render_width / (float)DISPLAY_WIDTH;
        float scale_y = (float)render_height / (float)DISPLAY_HEIGHT;
        float glyph_scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (glyph_scale <= 0.0f) {
            glyph_scale = 1.0f;
        }

        /* Create background color from config */
        uint32_t bg_color = ((current_config.bg_color_r << 16) |
                             (current_config.bg_color_g << 8) |
                             (current_config.bg_color_b));

        /* Clear framebuffer to configured background color */
        uint32_t *fb32 = (uint32_t *)back_buffer;
        uint32_t pixels = (drm.back.pitch / 4) * render_height;
        for (uint32_t i = 0; i < pixels; i++) {
            fb32[i] = bg_color;
        }

        /* Calculate scaled glyph height for vertical centering */
        uint32_t scaled_glyph_height = (uint32_t)(256.0f * glyph_scale + 0.5f);
        
        /* Calculate vertical center and apply offset */
        uint32_t center_y = (render_height > scaled_glyph_height) ?
                           ((render_height - scaled_glyph_height) / 2) : 0;
        uint32_t text_y_final = center_y + current_config.timecode_y_offset;
        /* Clamp to valid range */
        if (text_y_final > render_height - scaled_glyph_height) {
            text_y_final = render_height - scaled_glyph_height;
        }

        /* Calculate scaled string width for horizontal centering */
        /* Timecode is "HH.MM.SS" = 6 digits + 2 periods (periods overlay, spacing=0) */
        uint32_t base_string_width = 6 * DISPLAY_DIGIT_WIDTH;
        uint32_t scaled_string_width = (uint32_t)(base_string_width * glyph_scale + 0.5f);

        /* Calculate scaled digit width for background bounds */
        uint32_t scaled_digit_width = (uint32_t)(DISPLAY_DIGIT_WIDTH * glyph_scale + 0.5f);
        if (scaled_digit_width == 0) {
            scaled_digit_width = 1;
        }

        /* Center timecode, then clamp so full 8-digit background stays on screen */
        int32_t center_x = (render_width > scaled_string_width) ?
                           (int32_t)((render_width - scaled_string_width) / 2) : 0;
        int32_t text_x_final = center_x + current_config.timecode_x_offset;

        int32_t min_text_x = (int32_t)scaled_digit_width;
        int32_t max_text_x = (int32_t)render_width - (int32_t)(7 * scaled_digit_width);
        if (text_x_final < min_text_x) {
            text_x_final = min_text_x;
        }
        if (text_x_final > max_text_x) {
            text_x_final = max_text_x;
        }

        if (text_x_final < 0) {
            text_x_final = 0;
        }
        if ((uint32_t)text_x_final + scaled_string_width > render_width) {
            text_x_final = (int32_t)(render_width - scaled_string_width);
            if (text_x_final < 0) {
                text_x_final = 0;
            }
        }

        /* Background "8.8.8.8.8.8.8.8." positioned 1 digit width left of timecode */
        /* Config X offset centers the background relative to screen */
        uint32_t bg_x = ((uint32_t)text_x_final > scaled_digit_width) ?
            ((uint32_t)text_x_final - scaled_digit_width) : 0;
        
        /* Render background layer (unlit 7-segment grid) */
        font_blit_background_scaled(back_buffer, render_width, render_height, drm.back.pitch,
                        bg_x, text_y_final, glyph_scale);

        /* Get current time and render timecode */
        time_t now = time(NULL);
        if (now != last_time_display) {
            last_time_display = now;
        }

        char timecode_str[32];
        struct tm *tm_local = localtime(&now);

        /* Try to get LTC frame; fall back to system time */
        ltc_frame_t ltc_frame = {0};
        if (SHOW_SYSTEM_TIME || ltc_get_frame(&ltc, &ltc_frame) != 0) {
            ltc_frame_to_string_with_tz(&ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
        } else {
            ltc_frame_to_string(&ltc_frame, timecode_str, sizeof(timecode_str));
        }
        
        /* Render timecode with configured color and position */
        /* Use center-relative offsets for both X and Y */
        uint32_t text_x = (uint32_t)text_x_final;
        uint32_t text_y = text_y_final;
        uint32_t text_color = ((current_config.color_r << 16) |
                               (current_config.color_g << 8) |
                               (current_config.color_b));
        
        font_blit_string_scaled(back_buffer, render_width, render_height, drm.back.pitch,
                    text_x, text_y, timecode_str,
                    text_color, glyph_scale);

        /* Page flip (vblank-synced) */
        static int first_frame = 1;
        if (first_frame) {
            clock_gettime(CLOCK_BOOTTIME, &ts);
            printf("[MAIN] First frame ready for flip at boot+%.3f s\n", ts.tv_sec + ts.tv_nsec/1e9);
            fflush(stdout);
            first_frame = 0;
        }

        if (drm_page_flip_sync(&drm)) {
            fprintf(stderr, "[MAIN] Page flip failed\n");
            fflush(stderr);
            break;
        }

        frame_count++;

        /* Log status every ~5 seconds */
        if ((frame_count % (5 * drm.mode_vrefresh)) == 0) {
            printf("[MAIN] Frame %" PRIu64 ", Timecode: %s\n", frame_count, timecode_str);
            fflush(stdout);
        }

        /* Yield CPU briefly to allow GPIO edges to be captured */
        struct timespec ts = {0, 100000};  /* 100 µs */
        nanosleep(&ts, NULL);
    }

    printf("[MAIN] Shutting down...\n");

    /* Cleanup */
    config_watcher_cleanup(&config_watcher);
    gpio_cleanup(&gpio);
    drm_cleanup(&drm);

    printf("[MAIN] Cleanup complete. Exiting.\n");
    return EXIT_SUCCESS;
}

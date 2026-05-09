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

/*
 * Rotate a landscape ARGB8888 image (src_w x src_h) into a portrait
 * destination buffer (dst_w=src_h, dst_h=src_w), 90 degrees counter-clockwise.
 */
static void rotate_argb8888_90ccw(const uint8_t *src,
                                  uint32_t src_w,
                                  uint32_t src_h,
                                  uint32_t src_pitch,
                                  uint8_t *dst,
                                  uint32_t dst_pitch) {
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t *dst32 = (uint32_t *)dst;
    uint32_t src_stride = src_pitch / 4;
    uint32_t dst_stride = dst_pitch / 4;

    for (uint32_t y = 0; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            uint32_t pixel = src32[y * src_stride + x];
            uint32_t dx = y;
            uint32_t dy = src_w - 1 - x;
            dst32[dy * dst_stride + dx] = pixel;
        }
    }
}

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
    uint8_t *staging_buffer = NULL;
    size_t staging_size = 0;
    int logged_portrait_comp = 0;

    /* Option A: dirty-suppression tracking */
    char prev_timecode_str[32] = {0};
    uint64_t render_op_count = 0;   /* renders performed in current 5-s window */
    uint64_t skip_count = 0;        /* frames skipped (identical timecode) */
    uint64_t total_render_ns = 0;   /* accumulated render time (ns) */

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

        uint32_t fb_width = drm.back.hdisplay;
        uint32_t fb_height = drm.back.vdisplay;
        int portrait_mode = (fb_height > fb_width);

        /*
         * For portrait-only DSI modes (e.g. 480x1920), render into a logical
         * landscape surface and rotate into the real framebuffer.
         */
        uint32_t render_width = portrait_mode ? fb_height : fb_width;
        uint32_t render_height = portrait_mode ? fb_width : fb_height;
        uint32_t render_pitch = portrait_mode ? (render_width * 4) : drm.back.pitch;

        uint8_t *render_buffer = back_buffer;
        if (portrait_mode) {
            size_t needed = (size_t)render_pitch * (size_t)render_height;
            if (staging_size != needed) {
                uint8_t *new_buf = (uint8_t *)realloc(staging_buffer, needed);
                if (!new_buf) {
                    fprintf(stderr, "[MAIN] Failed to allocate portrait staging buffer (%zu bytes)\n", needed);
                    break;
                }
                staging_buffer = new_buf;
                staging_size = needed;
            }
            render_buffer = staging_buffer;

            if (!logged_portrait_comp) {
                printf("[MAIN] Portrait mode detected (%ux%u). Using logical render %ux%u with 90deg compensation.\n",
                       fb_width, fb_height, render_width, render_height);
                fflush(stdout);
                logged_portrait_comp = 1;
            }
        }

        float base_scale_x = (float)render_width / (float)DISPLAY_WIDTH;
        float base_scale_y = (float)render_height / (float)DISPLAY_HEIGHT;
        float base_scale = (base_scale_x < base_scale_y) ? base_scale_x : base_scale_y;

        /*
         * Choose glyph scale from target height, but clamp so:
         * 1) glyph height fits render height
         * 2) full 8-digit background ("8.8.8.8.8.8.8.8.") fits horizontally
         */
        float target_scale = (float)TIMECODE_FONT_HEIGHT / (float)FONT_GLYPH_HEIGHT;
        float max_scale_h = (float)render_height / (float)FONT_GLYPH_HEIGHT;
        float max_scale_bg = (float)render_width / (float)(8 * DISPLAY_DIGIT_WIDTH);
        float max_safe_scale = (max_scale_h < max_scale_bg) ? max_scale_h : max_scale_bg;

        float glyph_scale = target_scale;
        if (glyph_scale > max_safe_scale) {
            glyph_scale = max_safe_scale;
        }
        if (glyph_scale <= 0.0f) {
            glyph_scale = (base_scale > 0.0f) ? base_scale : 1.0f;
        }

        /* Compute timecode string first so we can skip render if unchanged */
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

        /* Option A: skip render and flip entirely when timecode is unchanged */
        int need_render = (strcmp(timecode_str, prev_timecode_str) != 0);
        if (need_render) {
            memcpy(prev_timecode_str, timecode_str, sizeof(prev_timecode_str));

            struct timespec t_render_start, t_render_end;
            clock_gettime(CLOCK_MONOTONIC, &t_render_start);

            /* Create background color from config */
            uint32_t bg_color = ((current_config.bg_color_r << 16) |
                                 (current_config.bg_color_g << 8) |
                                 (current_config.bg_color_b));

            /* Clear framebuffer to configured background color */
            uint32_t *fb32 = (uint32_t *)render_buffer;
            uint32_t pixels = (render_pitch / 4) * render_height;
            for (uint32_t i = 0; i < pixels; i++) {
                fb32[i] = bg_color;
            }

            /* Calculate scaled glyph height for vertical centering */
            uint32_t scaled_glyph_height = (uint32_t)(FONT_GLYPH_HEIGHT * glyph_scale + 0.5f);

            /* Calculate vertical center and apply offset */
            int32_t center_y = (render_height > scaled_glyph_height) ?
                               (int32_t)((render_height - scaled_glyph_height) / 2) : 0;
            int32_t text_y_final = center_y + current_config.timecode_y_offset;
            /* Clamp to valid range */
            int32_t min_text_y = 0;
            int32_t max_text_y = (int32_t)render_height - (int32_t)scaled_glyph_height;
            if (max_text_y < 0) {
                max_text_y = 0;
            }
            if (text_y_final < min_text_y) {
                text_y_final = min_text_y;
            }
            if (text_y_final > max_text_y) {
                text_y_final = max_text_y;
            }

            /* Calculate scaled string width for horizontal centering */
            /* Timecode is "HH.MM.SS.FF" = 8 digits + 3 periods (periods overlay, spacing=0) */
            uint32_t base_string_width = 8 * DISPLAY_DIGIT_WIDTH;
            uint32_t scaled_string_width = (uint32_t)(base_string_width * glyph_scale + 0.5f);

            /* Calculate scaled digit width for background bounds */
            uint32_t scaled_digit_width = (uint32_t)(DISPLAY_DIGIT_WIDTH * glyph_scale + 0.5f);
            if (scaled_digit_width == 0) {
                scaled_digit_width = 1;
            }

            /* Center timecode, then clamp so full background stays on screen */
            int32_t center_x = (render_width > scaled_string_width) ?
                               (int32_t)((render_width - scaled_string_width) / 2) : 0;
            int32_t text_x_final = center_x + current_config.timecode_x_offset;

            int32_t min_text_x = (int32_t)scaled_digit_width;
            int32_t max_text_x = (int32_t)render_width - (int32_t)(9 * scaled_digit_width);
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
            uint32_t bg_x = ((uint32_t)text_x_final > scaled_digit_width) ?
                ((uint32_t)text_x_final - scaled_digit_width) : 0;

            /* Render background layer (unlit 7-segment grid) */
            font_blit_background_scaled(render_buffer, render_width, render_height, render_pitch,
                            bg_x, text_y_final, glyph_scale);

            /* Render timecode */
            uint32_t text_x = (uint32_t)text_x_final;
            uint32_t text_y = (uint32_t)text_y_final;
            uint32_t text_color = ((current_config.color_r << 16) |
                                   (current_config.color_g << 8) |
                                   (current_config.color_b));

            font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                        text_x, text_y, timecode_str,
                        text_color, glyph_scale);

            if (portrait_mode) {
                rotate_argb8888_90ccw(render_buffer,
                                      render_width,
                                      render_height,
                                      render_pitch,
                                      back_buffer,
                                      drm.back.pitch);
            }

            clock_gettime(CLOCK_MONOTONIC, &t_render_end);
            total_render_ns += (uint64_t)(t_render_end.tv_sec - t_render_start.tv_sec) * 1000000000ULL
                             + (uint64_t)(t_render_end.tv_nsec - t_render_start.tv_nsec);
            render_op_count++;

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
        } else {
            skip_count++;
        }

        frame_count++;

        /* Log status every ~5 seconds */
        if ((frame_count % (5 * drm.mode_vrefresh)) == 0) {
            uint64_t avg_us = render_op_count > 0
                ? total_render_ns / render_op_count / 1000 : 0;
            printf("[MAIN] Frame %" PRIu64 ", TC: %s | rendered=%" PRIu64
                   " skipped=%" PRIu64 " avg_render=%" PRIu64 "us\n",
                   frame_count, timecode_str, render_op_count, skip_count, avg_us);
            fflush(stdout);
            render_op_count = 0;
            skip_count = 0;
            total_render_ns = 0;
        }

        /* Yield CPU briefly to allow GPIO edges to be captured */
        struct timespec ts = {0, 100000};  /* 100 µs */
        nanosleep(&ts, NULL);
    }

    printf("[MAIN] Shutting down...\n");

    /* Cleanup */
    free(staging_buffer);
    config_watcher_cleanup(&config_watcher);
    gpio_cleanup(&gpio);
    drm_cleanup(&drm);

    printf("[MAIN] Cleanup complete. Exiting.\n");
    return EXIT_SUCCESS;
}

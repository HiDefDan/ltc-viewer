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
    printf("[MAIN] Initial config loaded: TZ=%s, NTP=%s, Hz=%d\n",
           current_config.timezone, current_config.ntp_server, current_config.refresh_hz);

    /* Initialize DRM for framebuffer rendering with config refresh rate */
    ltc_drm_context_t drm = {0};
    uint32_t target_hz = (current_config.refresh_hz > 0) ? current_config.refresh_hz : TARGET_REFRESH_HZ;
    if (drm_init(&drm, target_hz)) {
        fprintf(stderr, "Failed to initialize DRM\n");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] DRM initialized: %u Hz (boot+%.3f s)\n", drm.mode_vrefresh, ts.tv_sec + ts.tv_nsec/1e9);

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
                printf("[DEBUG] New config loaded: Hz=%d\n", new_config.refresh_hz);
                printf("[DEBUG] Current config: Hz=%d\n", current_config.refresh_hz);
                fflush(stdout);
                
                /* Check if refresh rate changed */
                if (new_config.refresh_hz != current_config.refresh_hz && new_config.refresh_hz > 0) {
                    printf("[MAIN] Refresh rate changed from %d Hz to %d Hz, reinitializing DRM...\n",
                           current_config.refresh_hz, new_config.refresh_hz);
                    fflush(stdout);
                    drm_cleanup(&drm);
                    if (drm_init(&drm, new_config.refresh_hz) == 0) {
                        printf("[MAIN] DRM reinitialized successfully: %u Hz\n", drm.mode_vrefresh);
                        fflush(stdout);
                    } else {
                        fprintf(stderr, "[MAIN] Failed to reinitialize DRM with %d Hz, exiting\n", new_config.refresh_hz);
                        fflush(stderr);
                        should_exit = 1;
                    }
                } else {
                    printf("[DEBUG] No refresh rate change detected or invalid Hz\n");
                    fflush(stdout);
                }
                current_config = new_config;
                printf("[MAIN] Config reloaded: TZ=%s, NTP=%s, Hz=%d, Pos=(%d,%d), Color=(%d,%d,%d)\n",
                       current_config.timezone, current_config.ntp_server, current_config.refresh_hz,
                       current_config.timecode_x, current_config.timecode_y,
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

        /* Create background color from config */
        uint32_t bg_color = ((current_config.bg_color_r << 16) |
                             (current_config.bg_color_g << 8) |
                             (current_config.bg_color_b));

        /* Clear framebuffer to configured background color */
        uint32_t *fb32 = (uint32_t *)back_buffer;
        for (uint32_t i = 0; i < ((DISPLAY_WIDTH * DISPLAY_HEIGHT * 4) / 4); i++) {
            fb32[i] = bg_color;
        }

        /* Background "8.8.8.8.8.8.8.8." positioned 1 digit width left of timecode */
        uint32_t bg_x = (current_config.timecode_x > DISPLAY_DIGIT_WIDTH) ? 
                        (current_config.timecode_x - DISPLAY_DIGIT_WIDTH) : 0;
        
        /* Render background layer (unlit 7-segment grid) */
        font_blit_background(back_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, drm.back.pitch,
                            bg_x, current_config.timecode_y);

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
        uint32_t text_x = current_config.timecode_x;
        uint32_t text_y = current_config.timecode_y;
        uint32_t text_color = ((current_config.color_r << 16) |
                               (current_config.color_g << 8) |
                               (current_config.color_b));
        
        font_blit_string(back_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, drm.back.pitch,
                         text_x, text_y, timecode_str,
                         text_color);

        /* Page flip (vblank-synced) */
        static int first_frame = 1;
        if (first_frame) {
            clock_gettime(CLOCK_BOOTTIME, &ts);
            printf("[MAIN] First frame ready for flip at boot+%.3f s\n", ts.tv_sec + ts.tv_nsec/1e9);
            fflush(stdout);
            first_frame = 0;
        }

        if (drm_page_flip_sync(&drm)) {
            fprintf(stderr, "Page flip failed\n");
            break;
        }

        frame_count++;

        /* Log status every ~5 seconds */
        if ((frame_count % (5 * drm.mode_vrefresh)) == 0) {
            printf("[MAIN] Frame %" PRIu64 ", Timecode: %s\n", frame_count, timecode_str);
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

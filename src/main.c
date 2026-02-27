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

static volatile int should_exit = 0;

void signal_handler(int sig) {
    (void)sig;
    should_exit = 1;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("LTC Timecode Reader v1.0 (Raspberry Pi 5)\n");

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

    /* Initialize DRM for framebuffer rendering */
    ltc_drm_context_t drm = {0};
    if (drm_init(&drm, TARGET_REFRESH_HZ)) {
        fprintf(stderr, "Failed to initialize DRM\n");
        return EXIT_FAILURE;
    }

    printf("[MAIN] DRM initialized: %u Hz\n", drm.mode_vrefresh);

    /* Initialize bitmap font */
    if (font_init()) {
        fprintf(stderr, "Failed to initialize font\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    printf("[MAIN] Font initialized\n");

    /* Load background layer (unlit 7-segment grid) */
    font_load_background();

    /* Initialize LTC decoder */
    ltc_decoder_t ltc = {0};
    if (ltc_decoder_init(&ltc, LTC_FRAME_RATE)) {
        fprintf(stderr, "Failed to initialize LTC decoder\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    printf("[MAIN] LTC decoder initialized for %u fps\n", LTC_FRAME_RATE);

    /* Initialize GPIO for LTC edge capture */
    gpio_context_t gpio = {0};
    if (gpio_init(&gpio, GPIO_LTC_PIN)) {
        fprintf(stderr, "Failed to initialize GPIO\n");
        drm_cleanup(&drm);
        return EXIT_FAILURE;
    }

    printf("[MAIN] GPIO initialized on BCM%d\n", GPIO_LTC_PIN);

    /* Main render loop */
    uint64_t frame_count = 0;
    time_t last_time_display = 0;

    printf("[MAIN] Starting render loop...\n");

    while (!should_exit) {
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

        /* Clear framebuffer to black */
        uint32_t *fb32 = (uint32_t *)back_buffer;
        for (uint32_t i = 0; i < ((DISPLAY_WIDTH * DISPLAY_HEIGHT * 4) / 4); i++) {
            fb32[i] = COLOR_BLACK;
        }

        /* Render background layer (unlit 7-segment grid) */
        font_blit_background(back_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, drm.back.pitch,
                            BACKGROUND_X, TIMECODE_Y);

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
        
        /* Render timecode with 7-segment font */
        uint32_t text_x = TIMECODE_X;
        uint32_t text_y = TIMECODE_Y;
        
        font_blit_string(back_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, drm.back.pitch,
                         text_x, text_y, timecode_str,
                         COLOR_WHITE);

        /* Page flip (vblank-synced) */
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
    gpio_cleanup(&gpio);
    drm_cleanup(&drm);

    printf("[MAIN] Cleanup complete. Exiting.\n");
    return EXIT_SUCCESS;
}

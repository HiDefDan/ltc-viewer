#define _GNU_SOURCE
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
#include <pthread.h>
#include <rtaudio/rtaudio_c.h>

#include "drm.h"
#include "font.h"
#include "ltc-timecode.h"
#include "config.h"
#include "config-management.h"
#include "config-watcher.h"

static volatile int should_exit = 0;
static ltc_drm_context_t g_drm;

/* Shared state written by capture thread, read by main thread.
 * Protected by a mutex that is held only for a memcpy — never during DRM renders
 * or libltc decode, so the capture thread never blocks for more than ~1us. */
typedef struct {
    ltc_frame_t frame;
    int         fresh;      /* 1 = new frame since last main-thread read */
    time_t      last_seen;
    uint64_t    ingest_mono_ns;
    uint64_t    frame_mono_ns;
} ltc_shared_t;

typedef struct {
    rtaudio_t       rta;
    ltc_decoder_t  *decoder;
    ltc_shared_t   *shared;
    pthread_mutex_t *shared_mutex;
} rtaudio_ctx_t;

/* RtAudio input callback — runs in RtAudio's internal callback thread.
 * Receives SINT16 mono frames directly from HiFiBerry ADC.
 * No bit-shifting or channel extraction: RtAudio opens the device as
 * 1-channel S16_LE so samples arrive ready to feed straight to libltc. */
static int ltc_rtaudio_callback(void *out, void *in, unsigned int nframes,
                                double stream_time, rtaudio_stream_status_t status,
                                void *userdata)
{
    (void)out; (void)stream_time; (void)status;
    rtaudio_ctx_t *ctx = (rtaudio_ctx_t *)userdata;
    if (!in || nframes == 0) return 0;
    const int16_t *pcm = (const int16_t *)in;

    struct timespec ts_ingest;
    clock_gettime(CLOCK_MONOTONIC, &ts_ingest);
    uint64_t ingest_ns = (uint64_t)ts_ingest.tv_sec * 1000000000ULL + (uint64_t)ts_ingest.tv_nsec;

    /* Feed and decode outside the shared mutex — decoder is only ever
     * touched from this single callback thread. */
    ltc_feed_audio(ctx->decoder, pcm, nframes, 1);

    ltc_frame_t decoded;
    while (ltc_get_frame(ctx->decoder, &decoded) == 0) {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t mono_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + (uint64_t)ts_now.tv_nsec;

        /* Mutex held only for the tiny shared-result copy (~<1 µs). */
        pthread_mutex_lock(ctx->shared_mutex);
        ctx->shared->frame     = decoded;
        ctx->shared->fresh     = 1;
        ctx->shared->last_seen = time(NULL);
        ctx->shared->ingest_mono_ns = ingest_ns;
        ctx->shared->frame_mono_ns = mono_ns;
        pthread_mutex_unlock(ctx->shared_mutex);
    }
    return 0;
}

/* Enumerate RtAudio ALSA devices and return the HiFiBerry input id.
 * Falls back to the ALSA default input if not found by name. */
static unsigned int rta_find_hifiberry_input(rtaudio_t rta)
{
    int ndev = rtaudio_device_count(rta);
    unsigned int fallback = 0;
    for (int i = 0; i < ndev; i++) {
        unsigned int did = rtaudio_get_device_id(rta, i);
        rtaudio_device_info_t info = rtaudio_get_device_info(rta, did);
        if (info.input_channels == 0) continue;
        /* Match the ALSA card-id fragment present in both pre- and post-reboot
         * card numbering (sndrpihifiberry / HiFiBerry). */
        if (strstr(info.name, "hifiberry") || strstr(info.name, "HiFiBerry") ||
            strstr(info.name, "sndrpi")) {
            printf("[RTA] HiFiBerry input: %s (id=%u, %u ch)\n",
                   info.name, did, info.input_channels);
            return did;
        }
        if (fallback == 0) fallback = did;
    }
    unsigned int def = rtaudio_get_default_input_device(rta);
    printf("[RTA] Using default input device id=%u\n", def ? def : fallback);
    return def ? def : fallback;
}
/* Draw a tiny lowercase-style "df" badge using only the period glyph.
 * This keeps the overlay within the existing glyph set (digits + period). */
static void draw_df_badge(uint8_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                          uint32_t logical_width, uint32_t logical_height, int portrait_mode,
                          uint32_t x, uint32_t y, uint32_t color, float dot_scale)
{
    static const int d_pts[][2] = {
        {1,0},{2,0},
        {2,1},
        {1,2},{2,2},
        {1,3},{2,3},
        {1,4},{2,4}
    };
    static const int f_pts[][2] = {
        {0,0},{1,0},{2,0},
        {0,1},
        {0,2},{1,2},
        {0,3},
        {0,4}
    };

    uint32_t step = (uint32_t)(18.0f * dot_scale + 0.5f);
    if (step < 2) {
        step = 2;
    }

    for (size_t i = 0; i < sizeof(d_pts)/sizeof(d_pts[0]); i++) {
        uint32_t px = x + (uint32_t)d_pts[i][0] * step;
        uint32_t py = y + (uint32_t)d_pts[i][1] * step;
        if (portrait_mode) {
            font_blit_string_scaled_rot90ccw(fb, fb_width, fb_height, fb_pitch,
                                             logical_width, logical_height,
                                             px, py, ".", color, dot_scale);
        } else {
            font_blit_string_scaled(fb, fb_width, fb_height, fb_pitch, px, py, ".", color, dot_scale);
        }
    }

    uint32_t fx0 = x + 4 * step;
    for (size_t i = 0; i < sizeof(f_pts)/sizeof(f_pts[0]); i++) {
        uint32_t px = fx0 + (uint32_t)f_pts[i][0] * step;
        uint32_t py = y + (uint32_t)f_pts[i][1] * step;
        if (portrait_mode) {
            font_blit_string_scaled_rot90ccw(fb, fb_width, fb_height, fb_pitch,
                                             logical_width, logical_height,
                                             px, py, ".", color, dot_scale);
        } else {
            font_blit_string_scaled(fb, fb_width, fb_height, fb_pitch, px, py, ".", color, dot_scale);
        }
    }
}

static void copy_argb_rect(uint8_t *dst,
                           const uint8_t *src,
                           uint32_t fb_width,
                           uint32_t fb_height,
                           uint32_t pitch,
                           int32_t x,
                           int32_t y,
                           int32_t w,
                           int32_t h)
{
    if (!dst || !src || pitch < 4 || w <= 0 || h <= 0) {
        return;
    }

    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = x + w;
    int32_t y1 = y + h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)fb_width) x1 = (int32_t)fb_width;
    if (y1 > (int32_t)fb_height) y1 = (int32_t)fb_height;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    size_t row_bytes = (size_t)(x1 - x0) * 4u;
    uint32_t stride = pitch / 4;
    for (int32_t row = y0; row < y1; row++) {
        size_t off = ((size_t)row * (size_t)stride + (size_t)x0) * 4u;
        memcpy(dst + off, src + off, row_bytes);
    }
}

static uint32_t tc_index_24h(const ltc_frame_t *f, uint32_t fps)
{
    uint32_t sec_of_day = f->hours * 3600u + f->minutes * 60u + f->seconds;
    return sec_of_day * fps + (f->frame % fps);
}

static int32_t tc_signed_delta(const ltc_frame_t *from, const ltc_frame_t *to, uint32_t fps)
{
    uint32_t day_frames = 24u * 3600u * fps;
    uint32_t fi = tc_index_24h(from, fps);
    uint32_t ti = tc_index_24h(to, fps);
    uint32_t forward = (ti + day_frames - fi) % day_frames;
    uint32_t backward = (fi + day_frames - ti) % day_frames;
    return (forward <= backward) ? (int32_t)forward : -(int32_t)backward;
}

static ltc_frame_t tc_add_steps(const ltc_frame_t *f, uint32_t fps, uint32_t steps)
{
    ltc_frame_t out = *f;
    if (fps == 0) {
        return out;
    }

    uint32_t day_frames = 24u * 3600u * fps;
    uint32_t idx = tc_index_24h(f, fps);
    idx = (idx + steps) % day_frames;

    uint32_t sec_of_day = idx / fps;
    out.frame = idx % fps;
    out.hours = (sec_of_day / 3600u) % 24u;
    out.minutes = (sec_of_day / 60u) % 60u;
    out.seconds = sec_of_day % 60u;
    return out;
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

    /* Pin main thread to isolated core 3 when available. */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
            fprintf(stderr, "[MAIN] Warning: Failed to pin main thread to CPU 3\n");
            perror("[MAIN] sched_setaffinity");
        } else {
            printf("[MAIN] Main thread pinned to CPU 3\n");
        }
    }

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
    float target_hz = (current_config.refresh_hz > 0.0f) ? current_config.refresh_hz : (float)TARGET_REFRESH_HZ;
    if (drm_init(&g_drm, (uint32_t)current_config.display_width, (uint32_t)current_config.display_height, target_hz)) {
        fprintf(stderr, "Failed to initialize DRM\n");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] DRM initialized: %.2f Hz (boot+%.3f s)\n", g_drm.mode_refresh_hz, ts.tv_sec + ts.tv_nsec/1e9);

    /* Update current_config to reflect what DRM actually selected */
    current_config.display_width = g_drm.front.hdisplay;
    current_config.display_height = g_drm.front.vdisplay;
    current_config.refresh_hz = g_drm.mode_refresh_hz;

    /* Initialize bitmap font */
    if (font_init()) {
        fprintf(stderr, "Failed to initialize font\n");
        drm_cleanup(&g_drm);
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
        drm_cleanup(&g_drm);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] LTC decoder initialized for %u fps (boot+%.3f s)\n", LTC_FRAME_RATE, ts.tv_sec + ts.tv_nsec/1e9);
    fflush(stdout);

    pthread_mutex_t ltc_mutex;
    ltc_shared_t ltc_shared = {0};
    if (pthread_mutex_init(&ltc_mutex, NULL) != 0) {
        fprintf(stderr, "Failed to initialize LTC mutex\n");
        ltc_decoder_cleanup(&ltc);
        drm_cleanup(&g_drm);
        return EXIT_FAILURE;
    }

    rtaudio_ctx_t rtactx;
    memset(&rtactx, 0, sizeof(rtactx));
    rtactx.decoder      = &ltc;
    rtactx.shared       = &ltc_shared;
    rtactx.shared_mutex = &ltc_mutex;

    rtactx.rta = rtaudio_create(RTAUDIO_API_LINUX_ALSA);
    if (!rtactx.rta) {
        fprintf(stderr, "[RTA] Failed to create RtAudio ALSA instance\n");
        pthread_mutex_destroy(&ltc_mutex);
        ltc_decoder_cleanup(&ltc);
        drm_cleanup(&g_drm);
        return EXIT_FAILURE;
    }

    unsigned int rta_dev = rta_find_hifiberry_input(rtactx.rta);

    rtaudio_stream_parameters_t rta_in;
    memset(&rta_in, 0, sizeof(rta_in));
    rta_in.device_id     = rta_dev;
    rta_in.num_channels  = 1;   /* mono — RtAudio extracts channel 0 from hardware */
    rta_in.first_channel = 0;

    unsigned int rta_buf = RTAUDIO_CAPTURE_PERIOD_FRAMES;

    rtaudio_stream_options_t rta_opts;
    memset(&rta_opts, 0, sizeof(rta_opts));
    rta_opts.flags       = RTAUDIO_FLAGS_MINIMIZE_LATENCY | RTAUDIO_FLAGS_SCHEDULE_REALTIME;
    rta_opts.num_buffers = 4;
    rta_opts.priority    = 80;
    strncpy(rta_opts.name, "ltc-timecode", sizeof(rta_opts.name) - 1);

    int rta_rc = rtaudio_open_stream(rtactx.rta,
                                     NULL, &rta_in,
                                     RTAUDIO_FORMAT_SINT16,
                                     ALSA_CAPTURE_RATE,
                                     &rta_buf,
                                     ltc_rtaudio_callback,
                                     &rtactx,
                                     &rta_opts,
                                     NULL);
    if (rta_rc != RTAUDIO_ERROR_NONE) {
        fprintf(stderr, "[RTA] Open stream failed: %s\n", rtaudio_error(rtactx.rta));
        rtaudio_destroy(rtactx.rta);
        pthread_mutex_destroy(&ltc_mutex);
        ltc_decoder_cleanup(&ltc);
        drm_cleanup(&g_drm);
        return EXIT_FAILURE;
    }
    rtaudio_start_stream(rtactx.rta);

    clock_gettime(CLOCK_BOOTTIME, &ts);
    fflush(stdout);
    printf("[RTA] Capture started: device=%u buf=%u frames S16 mono 48kHz (boot+%.3f s)\n",
           rta_dev, rta_buf, ts.tv_sec + ts.tv_nsec/1e9);

    /* Main render loop */
    uint64_t frame_count = 0;
    time_t last_time_display = 0;
    int logged_portrait_direct = 0;
    uint8_t *static_layer_buffer = NULL;
    size_t static_layer_size = 0;
    int static_layer_valid = 0;
    uint32_t static_fb_width = 0;
    uint32_t static_fb_height = 0;
    uint32_t static_render_pitch = 0;
    uint32_t static_render_width = 0;
    uint32_t static_render_height = 0;
    int static_portrait_mode = 0;
    uint32_t static_bg_color = 0;
    uint32_t static_bg_x = 0;
    uint32_t static_text_y = 0;
    uint32_t static_glyph_scale_key = 0;

    /* Option A: dirty-suppression tracking */
    char prev_timecode_str[32] = {0};
    uint64_t render_op_count = 0;   /* renders performed in current 5-s window */
    uint64_t skip_count = 0;        /* frames skipped (identical timecode) */
    uint64_t total_render_ns = 0;   /* accumulated render time (ns) */
    uint64_t total_latency_us = 0;  /* accumulated decode->display latency (us) */
    uint64_t latency_samples = 0;   /* decode->display samples in current 5-s window */
    uint64_t total_ingest_to_decode_us = 0;   /* accumulated ADC ingest->decode latency (us) */
    uint64_t ingest_to_decode_samples = 0;
    uint64_t total_ingest_to_display_us = 0;  /* accumulated ADC ingest->display latency (us) */
    uint64_t ingest_to_display_samples = 0;
    uint64_t last_render_mono_ns = 0;
    ltc_frame_t last_ltc_frame = {0};
    ltc_frame_t target_ltc_frame = {0};
    uint64_t target_ltc_ingest_ns = 0;
    uint64_t target_ltc_mono_ns = 0;
    int has_ltc_frame = 0;
    int has_target_ltc_frame = 0;
    time_t last_ltc_seen = 0;
    struct timespec last_smooth_ts = {0};
    int smooth_clock_init = 0;
    double smooth_budget_frames = 0.0;

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
                                        drm_cleanup(&g_drm);
                    float new_target_hz = new_config.refresh_hz;
                                            if (drm_init(&g_drm, (uint32_t)new_config.display_width, (uint32_t)new_config.display_height, new_target_hz) == 0) {
                       printf("[MAIN] DRM reinitialized successfully: %ux%u @ %.2f Hz\n",
                                                     g_drm.front.hdisplay, g_drm.front.vdisplay, g_drm.mode_refresh_hz);
                        fflush(stdout);
                        /* Update new_config to reflect what DRM actually selected */
                                                new_config.display_width = g_drm.front.hdisplay;
                                                new_config.display_height = g_drm.front.vdisplay;
                                                new_config.refresh_hz = g_drm.mode_refresh_hz;
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

        /* Get back buffer and render */
        uint8_t *back_buffer = drm_get_back_buffer(&g_drm);
        if (!back_buffer) {
            fprintf(stderr, "Failed to get back buffer\n");
            break;
        }

        uint32_t fb_width = g_drm.back.hdisplay;
        uint32_t fb_height = g_drm.back.vdisplay;
        int portrait_mode = (fb_height > fb_width);

        /*
         * For portrait-only DSI modes (e.g. 480x1920), render into a logical
         * landscape surface and rotate into the real framebuffer.
         */
        uint32_t render_width = portrait_mode ? fb_height : fb_width;
        uint32_t render_height = portrait_mode ? fb_width : fb_height;
        uint32_t render_pitch = g_drm.back.pitch;
        uint8_t *render_buffer = back_buffer;
        if (portrait_mode && !logged_portrait_direct) {
            printf("[MAIN] Portrait mode detected (%ux%u). Rendering directly with logical canvas %ux%u (no full-frame rotate).\n",
                   fb_width, fb_height, render_width, render_height);
            fflush(stdout);
            logged_portrait_direct = 1;
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

        /* Decode selection:
         * 1) Show LTC immediately when a frame is available.
         * 2) On LTC loss, either hold last frame or restore ToD after timeout.
         */
        /* Collect LTC frame decoded by capture thread. Mutex held < 1us. */
        ltc_frame_t live_ltc_frame = {0};
        uint64_t live_ltc_ingest_ns = 0;
        uint64_t live_ltc_mono_ns = 0;
        int got_ltc = 0;
        pthread_mutex_lock(&ltc_mutex);
        if (ltc_shared.fresh) {
            live_ltc_frame = ltc_shared.frame;
            live_ltc_ingest_ns = ltc_shared.ingest_mono_ns;
            live_ltc_mono_ns = ltc_shared.frame_mono_ns;
            ltc_shared.fresh = 0;
            got_ltc = 1;
        }
        pthread_mutex_unlock(&ltc_mutex);

        float detected_ltc_fps = ltc_get_detected_fps(&ltc); /* benign race for UI */
        uint32_t nominal_ltc_fps = ltc_get_nominal_fps(&ltc); /* benign race for UI */
        uint64_t ltc_gap_count = ltc_get_gap_count(&ltc); /* benign race for UI */

        if (got_ltc) {
            has_ltc_frame = 1;
            last_ltc_seen = now;
            target_ltc_frame = live_ltc_frame;
            target_ltc_ingest_ns = live_ltc_ingest_ns;
            target_ltc_mono_ns = live_ltc_mono_ns;
            has_target_ltc_frame = 1;
            if (!smooth_clock_init) {
                clock_gettime(CLOCK_MONOTONIC, &last_smooth_ts);
                smooth_clock_init = 1;
            }
            if (last_ltc_frame.hours == 0 && last_ltc_frame.minutes == 0 &&
                last_ltc_frame.seconds == 0 && last_ltc_frame.frame == 0) {
                last_ltc_frame = target_ltc_frame;
            }
        }

        if (has_ltc_frame) {
            time_t loss_age = now - last_ltc_seen;
            if (loss_age >= current_config.dsi_ltc_loss_timeout_sec) {
                /* No fresh LTC for timeout window: force non-live state so
                 * display returns to ToD/green instead of holding stale frame. */
                has_ltc_frame = 0;
                has_target_ltc_frame = 0;
                target_ltc_ingest_ns = 0;
                target_ltc_mono_ns = 0;
                smooth_budget_frames = 0.0;
                ltc_frame_to_string_with_tz(&last_ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
            } else {
                if (has_target_ltc_frame && got_ltc) {
                    uint32_t fps_for_smooth = nominal_ltc_fps ? nominal_ltc_fps : LTC_FRAME_RATE;
                    int32_t delta = tc_signed_delta(&last_ltc_frame, &target_ltc_frame, fps_for_smooth);

                    if (!smooth_clock_init) {
                        clock_gettime(CLOCK_MONOTONIC, &last_smooth_ts);
                        smooth_clock_init = 1;
                    }

                    struct timespec now_smooth_ts;
                    clock_gettime(CLOCK_MONOTONIC, &now_smooth_ts);
                    double elapsed_s = (double)(now_smooth_ts.tv_sec - last_smooth_ts.tv_sec)
                                     + (double)(now_smooth_ts.tv_nsec - last_smooth_ts.tv_nsec) / 1e9;
                    if (elapsed_s < 0.0) {
                        elapsed_s = 0.0;
                    }
                    if (elapsed_s > 0.25) {
                        elapsed_s = 0.25;
                    }
                    last_smooth_ts = now_smooth_ts;

                    /* Advance strictly by elapsed real time so displayed LTC
                     * cannot run faster than source cadence. */
                    smooth_budget_frames += elapsed_s * (double)fps_for_smooth;

                    /* Keep only a tiny reserve to avoid bursty catch-up. */
                    double max_budget = 2.0;
                    if (smooth_budget_frames > max_budget) {
                        smooth_budget_frames = max_budget;
                    }

                    uint32_t allowed_steps = (uint32_t)smooth_budget_frames;
                    uint32_t abs_delta = (delta >= 0) ? (uint32_t)delta : (uint32_t)(-delta);

                    /* Snap on backward/large discontinuities (loop boundaries, seeks,
                     * bad jumps) so smoothing never wraps forward through hours. */
                    if (delta < 0 || abs_delta > (fps_for_smooth * 2u)) {
                        last_ltc_frame = target_ltc_frame;
                        smooth_budget_frames = 0.0;
                    } else if (delta > 0) {
                        if (allowed_steps > 0) {
                            uint32_t step = (abs_delta < allowed_steps) ? abs_delta : allowed_steps;
                            last_ltc_frame = tc_add_steps(&last_ltc_frame, fps_for_smooth, step);
                            last_ltc_frame.drop_frame = target_ltc_frame.drop_frame;
                            smooth_budget_frames -= (double)step;
                        }
                    } else if (smooth_budget_frames > 1.0) {
                        /* Prevent budget buildup while already in sync. */
                        smooth_budget_frames = 1.0;
                    }
                }
                ltc_frame_to_string(&last_ltc_frame, timecode_str, sizeof(timecode_str));
            }
        } else {
            ltc_frame_to_string_with_tz(&last_ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
        }

        /* Consider LTC live for one second after the latest decoded frame. */
        int ltc_live = has_ltc_frame && ((now - last_ltc_seen) <= 1);

        /* Only render/flip on fresh LTC data, and only when visible text changed. */
        int need_render = got_ltc && (strcmp(timecode_str, prev_timecode_str) != 0);
        if (need_render) {
            struct timespec ts_now_mono;
            clock_gettime(CLOCK_MONOTONIC, &ts_now_mono);
            uint64_t now_mono_ns = (uint64_t)ts_now_mono.tv_sec * 1000000000ULL + (uint64_t)ts_now_mono.tv_nsec;

            uint64_t min_interval_ns;
            if (ltc_live) {
                float cadence_fps = detected_ltc_fps;
                if (cadence_fps < 10.0f || cadence_fps > 120.0f) {
                    cadence_fps = (float)(nominal_ltc_fps ? nominal_ltc_fps : LTC_FRAME_RATE);
                }
                if (cadence_fps < 1.0f) {
                    cadence_fps = (float)LTC_FRAME_RATE;
                }
                min_interval_ns = (uint64_t)(1000000000.0 / (double)cadence_fps);
            } else {
                min_interval_ns = 10000000ULL; /* centisecond fallback */
            }

            if (last_render_mono_ns > 0 && (now_mono_ns - last_render_mono_ns) < min_interval_ns) {
                need_render = 0;
            }
        }
        if (need_render) {
            memcpy(prev_timecode_str, timecode_str, sizeof(prev_timecode_str));

            struct timespec t_render_start, t_render_end;
            clock_gettime(CLOCK_MONOTONIC, &t_render_start);

            /* Create background color from config */
            uint32_t bg_color = ((current_config.bg_color_r << 16) |
                                 (current_config.bg_color_g << 8) |
                                 (current_config.bg_color_b));

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

            /* Cache static scene: solid background + unlit 7-segment layer. */
            uint32_t glyph_scale_key = (uint32_t)(glyph_scale * 10000.0f + 0.5f);
            size_t required_static_size = (size_t)render_pitch * (size_t)fb_height;
            int static_layer_needs_rebuild = !static_layer_valid ||
                static_fb_width != fb_width ||
                static_fb_height != fb_height ||
                static_render_pitch != render_pitch ||
                static_render_width != render_width ||
                static_render_height != render_height ||
                static_portrait_mode != portrait_mode ||
                static_bg_color != bg_color ||
                static_bg_x != bg_x ||
                static_text_y != (uint32_t)text_y_final ||
                static_glyph_scale_key != glyph_scale_key;

            if (static_layer_size != required_static_size) {
                uint8_t *new_static = (uint8_t *)realloc(static_layer_buffer, required_static_size);
                if (!new_static) {
                    fprintf(stderr, "[MAIN] Failed to allocate static layer buffer (%zu bytes)\n",
                            required_static_size);
                    break;
                }
                static_layer_buffer = new_static;
                static_layer_size = required_static_size;
                static_layer_needs_rebuild = 1;
            }

            if (static_layer_needs_rebuild) {
                uint32_t *static_fb32 = (uint32_t *)static_layer_buffer;
                uint32_t static_pixels = (render_pitch / 4) * fb_height;
                for (uint32_t i = 0; i < static_pixels; i++) {
                    static_fb32[i] = bg_color;
                }

                if (portrait_mode) {
                    font_blit_background_scaled_rot90ccw(static_layer_buffer, fb_width, fb_height, render_pitch,
                                                         render_width, render_height,
                                                         bg_x, text_y_final, glyph_scale);
                } else {
                    font_blit_background_scaled(static_layer_buffer, render_width, render_height, render_pitch,
                                                bg_x, text_y_final, glyph_scale);
                }

                static_layer_valid = 1;
                static_fb_width = fb_width;
                static_fb_height = fb_height;
                static_render_pitch = render_pitch;
                static_render_width = render_width;
                static_render_height = render_height;
                static_portrait_mode = portrait_mode;
                static_bg_color = bg_color;
                static_bg_x = bg_x;
                static_text_y = (uint32_t)text_y_final;
                static_glyph_scale_key = glyph_scale_key;
            }

            /* Restore from static cache:
             * - full frame on static rebuilds, debug overlay mode, or landscape fallback
             * - otherwise just the dynamic timecode bounding region */
            if (static_layer_needs_rebuild || current_config.debug_overlay_enabled || !portrait_mode) {
                memcpy(render_buffer, static_layer_buffer, static_layer_size);
            } else {
                int32_t dirty_x = text_y_final;
                int32_t dirty_y = (int32_t)render_width - (text_x_final + (int32_t)scaled_string_width);
                int32_t dirty_w = (int32_t)scaled_glyph_height;
                int32_t dirty_h = (int32_t)scaled_string_width;
                copy_argb_rect(render_buffer, static_layer_buffer,
                               fb_width, fb_height, render_pitch,
                               dirty_x, dirty_y, dirty_w, dirty_h);
            }

            /* Render timecode */
            uint32_t text_x = (uint32_t)text_x_final;
            uint32_t text_y = (uint32_t)text_y_final;
            uint32_t text_color;
            if (ltc_live) {
                /* Live incoming LTC is always shown in red for at-a-glance status. */
                text_color = 0x00FF0000;
            } else {
                text_color = ((current_config.color_r << 16) |
                              (current_config.color_g << 8) |
                              (current_config.color_b));
            }

            if (portrait_mode) {
                font_blit_string_scaled_rot90ccw(render_buffer, fb_width, fb_height, render_pitch,
                                                 render_width, render_height,
                                                 text_x, text_y, timecode_str,
                                                 text_color, glyph_scale);
            } else {
                font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                                        text_x, text_y, timecode_str,
                                        text_color, glyph_scale);
            }

            if (current_config.debug_overlay_enabled) {
                /* Compact overlay: measured_fps.nominal_fps.expected_fps.gap_mod_100
                 * Example: 23.98.24.30.03
                 * If nominal != expected, color shifts to orange to call out rate mismatch. */
                int fps_int = (int)detected_ltc_fps;
                int fps_frac = (int)llround((detected_ltc_fps - (float)fps_int) * 100.0f);
                if (fps_frac < 0) {
                    fps_frac = 0;
                }
                if (fps_frac > 99) {
                    fps_frac = 99;
                }

                char overlay_str[32];
                unsigned int df_flag = (ltc_live && last_ltc_frame.drop_frame) ? 1u : 0u;
                unsigned int gap_mod = (unsigned int)(ltc_gap_count % 100u);
                unsigned int expected_fps = LTC_FRAME_RATE;
                snprintf(overlay_str, sizeof(overlay_str), "%02d.%02d.%02u.%02u.%02u",
                         fps_int, fps_frac, nominal_ltc_fps, expected_fps, gap_mod);

                float overlay_scale = glyph_scale * 0.23f;
                if (overlay_scale < 0.08f) {
                    overlay_scale = 0.08f;
                }
                uint32_t overlay_x = 12;
                uint32_t overlay_y = 12;
                uint32_t overlay_color = (nominal_ltc_fps != 0 && nominal_ltc_fps != expected_fps)
                    ? 0x00FF8800 : 0x00FFFF00;
                if (portrait_mode) {
                    font_blit_string_scaled_rot90ccw(render_buffer, fb_width, fb_height, render_pitch,
                                                     render_width, render_height,
                                                     overlay_x, overlay_y, overlay_str,
                                                     overlay_color, overlay_scale);
                } else {
                    font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                                            overlay_x, overlay_y, overlay_str,
                                            overlay_color, overlay_scale);
                }

                if (df_flag) {
                    uint32_t df_x = overlay_x + (uint32_t)(16.0f * DISPLAY_DIGIT_WIDTH * overlay_scale);
                    draw_df_badge(render_buffer, fb_width, fb_height, render_pitch,
                                  render_width, render_height, portrait_mode,
                                  df_x, overlay_y,
                                  overlay_color, overlay_scale * 0.72f);
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &t_render_end);
            last_render_mono_ns = (uint64_t)t_render_end.tv_sec * 1000000000ULL + (uint64_t)t_render_end.tv_nsec;
            total_render_ns += (uint64_t)(t_render_end.tv_sec - t_render_start.tv_sec) * 1000000000ULL
                             + (uint64_t)(t_render_end.tv_nsec - t_render_start.tv_nsec);
            render_op_count++;

            if (ltc_live && target_ltc_mono_ns > 0) {
                uint64_t now_mono_ns = (uint64_t)t_render_end.tv_sec * 1000000000ULL + (uint64_t)t_render_end.tv_nsec;
                if (now_mono_ns >= target_ltc_mono_ns) {
                    total_latency_us += (now_mono_ns - target_ltc_mono_ns) / 1000ULL;
                    latency_samples++;
                }

                if (target_ltc_ingest_ns > 0 && target_ltc_mono_ns >= target_ltc_ingest_ns) {
                    total_ingest_to_decode_us += (target_ltc_mono_ns - target_ltc_ingest_ns) / 1000ULL;
                    ingest_to_decode_samples++;
                }

                if (target_ltc_ingest_ns > 0 && now_mono_ns >= target_ltc_ingest_ns) {
                    total_ingest_to_display_us += (now_mono_ns - target_ltc_ingest_ns) / 1000ULL;
                    ingest_to_display_samples++;
                }
            }

            /* Page flip (vblank-synced) */
            static int first_frame = 1;
            if (first_frame) {
                clock_gettime(CLOCK_BOOTTIME, &ts);
                printf("[MAIN] First frame ready for flip at boot+%.3f s\n", ts.tv_sec + ts.tv_nsec/1e9);
                fflush(stdout);
                first_frame = 0;
            }

            if (drm_page_flip_sync(&g_drm)) {
                fprintf(stderr, "[MAIN] Page flip failed\n");
                fflush(stderr);
                break;
            }
        } else {
            skip_count++;
        }

        frame_count++;

        /* Log status every ~5 seconds */
        if ((frame_count % (5 * g_drm.mode_vrefresh)) == 0) {
            uint64_t avg_us = render_op_count > 0
                ? total_render_ns / render_op_count / 1000 : 0;
            uint64_t avg_decode_to_display_us = latency_samples > 0
                ? total_latency_us / latency_samples : 0;
            uint64_t avg_ingest_to_decode_us = ingest_to_decode_samples > 0
                ? total_ingest_to_decode_us / ingest_to_decode_samples : 0;
            uint64_t avg_ingest_to_display_us = ingest_to_display_samples > 0
                ? total_ingest_to_display_us / ingest_to_display_samples : 0;
            printf("[MAIN] Frame %" PRIu64 ", TC: %s | rendered=%" PRIu64
                   " skipped=%" PRIu64 " avg_render=%" PRIu64 "us ltc_fps=%.2f "
                   "adc_dec_lat_us=%" PRIu64 " dec_disp_lat_us=%" PRIu64 " adc_disp_lat_us=%" PRIu64 "\n",
                   frame_count, timecode_str, render_op_count, skip_count, avg_us, detected_ltc_fps,
                   avg_ingest_to_decode_us, avg_decode_to_display_us, avg_ingest_to_display_us);
            fflush(stdout);
            render_op_count = 0;
            skip_count = 0;
            total_render_ns = 0;
            total_latency_us = 0;
            latency_samples = 0;
            total_ingest_to_decode_us = 0;
            ingest_to_decode_samples = 0;
            total_ingest_to_display_us = 0;
            ingest_to_display_samples = 0;
        }

        /* Adaptive pacing to avoid busy-spin when content is unchanged. */
        {
            uint64_t target_period_ns;
            if (ltc_live) {
                float cadence_fps = detected_ltc_fps;
                if (cadence_fps < 10.0f || cadence_fps > 120.0f) {
                    cadence_fps = (float)(nominal_ltc_fps ? nominal_ltc_fps : LTC_FRAME_RATE);
                }
                if (cadence_fps < 1.0f) {
                    cadence_fps = (float)LTC_FRAME_RATE;
                }
                target_period_ns = (uint64_t)(1000000000.0 / (double)cadence_fps);
            } else {
                /* ToD fallback includes centiseconds, so target 100 Hz updates. */
                target_period_ns = 10000000ULL;
            }

            uint64_t sleep_ns = need_render ? (target_period_ns / 10ULL) : (target_period_ns / 4ULL);

            /* Keep checks responsive for config updates and LTC lock changes. */
            if (sleep_ns < 500000ULL) {
                sleep_ns = 500000ULL;      /* 0.5 ms min */
            }
            if (sleep_ns > 8000000ULL) {
                sleep_ns = 8000000ULL;     /* 8 ms max */
            }

            struct timespec ts_sleep = {
                .tv_sec = (time_t)(sleep_ns / 1000000000ULL),
                .tv_nsec = (long)(sleep_ns % 1000000000ULL)
            };
            nanosleep(&ts_sleep, NULL);
        }
    }

    printf("[MAIN] Shutting down...\n");

    /* Cleanup */
    if (rtactx.rta) {
        if (rtaudio_is_stream_running(rtactx.rta))
            rtaudio_abort_stream(rtactx.rta);
        if (rtaudio_is_stream_open(rtactx.rta))
            rtaudio_close_stream(rtactx.rta);
        rtaudio_destroy(rtactx.rta);
    }
    pthread_mutex_destroy(&ltc_mutex);
    ltc_decoder_cleanup(&ltc);
    free(static_layer_buffer);
    config_watcher_cleanup(&config_watcher);
    drm_cleanup(&g_drm);

    printf("[MAIN] Cleanup complete. Exiting.\n");
    return EXIT_SUCCESS;
}

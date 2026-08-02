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
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <limits.h>
#include <rtaudio/rtaudio_c.h>

#include "display-backend.h"
#include "font.h"
#include "ltc-timecode.h"
#include "config.h"
#include "config-management.h"
#include "config-watcher.h"

static volatile int should_exit = 0;

/* Definition for the extern declared in config.h. */
int g_log_verbose = 0;

/* Shutdown hard-exit backstop (spawned at the top of main()'s shutdown
 * path, before any teardown call runs). Root-caused 2026-08-02 via
 * checkpoint logging: a full `poweroff`/`halt` reliably hangs the entire
 * machine — not just this process — inside rtaudio_abort_stream() the
 * instant it's called, most likely kernel-level ALSA lock contention with
 * alsa-restore.service, which only runs concurrently during a real
 * multi-service shutdown (never during a standalone `systemctl stop`,
 * which is why that path "works", just slowly). Earlier suspects (a GBM/
 * EGL DRM wait, an RT-priority/RCU-isolation interaction) were both ruled
 * out by the same logging — the process never got past the very first
 * teardown call. SIGKILL cannot unstick an in-kernel wait, so an
 * unguarded hang here holds the process open past TimeoutStopSec until
 * the Pi's hardware watchdog (armed by RPi OS's stock
 * 40-rpi-enable-watchdog.conf, independent of anything in this codebase)
 * fires a hard SoC reset — which looks exactly like an unwanted reboot on
 * `halt`/`poweroff`, because the reset happens before the kernel ever
 * reaches the actual power-off instruction. Bounding the *whole* teardown
 * sequence (not just the step known to hang today) means any future stuck
 * call is caught the same way. */
static volatile int g_shutdown_done = 0;

/* Diagnostic checkpoint logging for the shutdown path (2026-08-02
 * investigation): pins down exactly which call the process is in when a
 * full-system poweroff stalls, since "before this line" vs "after it"
 * is otherwise invisible in the journal. Remove once shutdown is solid. */
static void log_shutdown_checkpoint(const char *label) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    fprintf(stderr, "[SHUTDOWN-CKPT] %s (boot+%.3f s)\n", label, ts.tv_sec + ts.tv_nsec / 1e9);
    fflush(stderr);
}

static void *shutdown_watchdog_thread(void *arg) {
    (void)arg;
    struct timespec ts = { 3, 0 }; /* generous vs. the ~0.1-0.2s this normally takes end-to-end */
    nanosleep(&ts, NULL);
    if (!g_shutdown_done) {
        fprintf(stderr, "[MAIN] Shutdown did not complete within 3s (stuck teardown call) — forcing exit\n");
        fflush(stderr);
        _exit(0);
    }
    return NULL;
}
static display_backend_t g_backend;
static volatile uint64_t g_rta_input_overflow = 0;
static volatile uint64_t g_rta_output_underflow = 0;
static volatile long g_gpu_clock_mhz_x10 = 0;
static volatile int g_gpu_clock_thread_running = 0;
static pthread_t g_gpu_clock_thread;

/* Shared state written by capture thread, read by main thread.
 * Protected by a mutex that is held only for a memcpy — never during DRM renders
 * or libltc decode, so the capture thread never blocks for more than ~1us. */
typedef struct {
    ltc_frame_t frame;
    int         fresh;      /* 1 = new frame since last main-thread read */
    time_t      last_seen;
    uint64_t    ingest_mono_ns;
    uint64_t    frame_mono_ns;
    uint64_t    frame_start_est_ns;
    uint64_t    frame_end_est_ns;
} ltc_shared_t;

typedef struct {
    rtaudio_t       rta;
    ltc_decoder_t  *decoder;
    ltc_shared_t   *shared;
    pthread_mutex_t *shared_mutex;
} rtaudio_ctx_t;

static unsigned int read_env_u32(const char *name,
                                 unsigned int fallback,
                                 unsigned int min_v,
                                 unsigned int max_v)
{
    const char *s = getenv(name);
    if (!s || !*s) {
        return fallback;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(s, &end, 10);
    if (end == s || *end != '\0') {
        return fallback;
    }

    if (parsed < min_v) {
        parsed = min_v;
    }
    if (parsed > max_v) {
        parsed = max_v;
    }
    return (unsigned int)parsed;
}

static long parse_gpu_v3d_clock_mhz_x10(const char *buf)
{
    unsigned long long cycles = 0;
    double mhz = 0.0;
    if (sscanf(buf, "cycles: %llu (%lf Mhz)", &cycles, &mhz) >= 1) {
        if (mhz > 0.0) {
            return (long)(mhz * 10.0 + 0.5);
        }
        if (cycles > 0ULL) {
            return (long)((double)cycles / 100000.0 + 0.5);
        }
    }

    const char *p = strstr(buf, "Mhz");
    if (p) {
        const char *q = p;
        while (q > buf && (*(q - 1) == ' ' || *(q - 1) == '\t')) {
            q--;
        }
        double result = 0.0;
        if (sscanf(q, "%lf", &result) == 1) {
            return (long)(result * 10.0 + 0.5);
        }
    }

    return 0;
}

static void *gpu_clock_reader_thread(void *arg)
{
    (void)arg;
    const char *path = "/sys/kernel/debug/dri/1002000000.v3d/measure_clock";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    char buf[256];
    struct timespec ts = {1, 0};
    while (g_gpu_clock_thread_running) {
        ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            long x10 = parse_gpu_v3d_clock_mhz_x10(buf);
            if (x10 > 0) {
                g_gpu_clock_mhz_x10 = x10;
            }
        }
        nanosleep(&ts, NULL);
    }

    close(fd);
    return NULL;
}

/* RtAudio input callback — runs in RtAudio's internal callback thread.
 * Receives SINT16 mono frames directly from HiFiBerry ADC.
 * No bit-shifting or channel extraction: RtAudio opens the device as
 * 1-channel S16_LE so samples arrive ready to feed straight to libltc. */
static int ltc_rtaudio_callback(void *out, void *in, unsigned int nframes,
                                double stream_time, rtaudio_stream_status_t status,
                                void *userdata)
{
    (void)out;
    (void)stream_time;
    if (status & RTAUDIO_STATUS_INPUT_OVERFLOW) {
        g_rta_input_overflow++;
    }
    if (status & RTAUDIO_STATUS_OUTPUT_UNDERFLOW) {
        g_rta_output_underflow++;
    }
    rtaudio_ctx_t *ctx = (rtaudio_ctx_t *)userdata;
    if (!in || nframes == 0) return 0;
    const int16_t *pcm = (const int16_t *)in;

    struct timespec ts_ingest;
    clock_gettime(CLOCK_MONOTONIC, &ts_ingest);
    uint64_t ingest_ns = (uint64_t)ts_ingest.tv_sec * 1000000000ULL + (uint64_t)ts_ingest.tv_nsec;

    /* Feed and decode outside the shared mutex — decoder is only ever
     * touched from this single callback thread. */
    ltc_feed_audio(ctx->decoder, pcm, nframes, 1);
    uint64_t callback_end_sample = ctx->decoder->sample_pos;

    /* Hard cap on the drain loop: the libltc queue holds at most 32 frames,
     * so anything past that means a state bug is feeding us frames forever —
     * and an unbounded loop here runs at RT priority on the isolated core,
     * where it starves the whole system (observed 2026-08-01: RR-80 spin →
     * watchdog reboot). Bail out and let the next callback continue. */
    int drain_budget = 64;
    ltc_frame_t decoded;
    while (drain_budget-- > 0 && ltc_get_frame(ctx->decoder, &decoded) == 0) {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        uint64_t mono_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + (uint64_t)ts_now.tv_nsec;
        uint64_t frame_end_est_ns = ingest_ns;
        uint64_t frame_start_est_ns = ingest_ns;

        if (callback_end_sample >= decoded.sample_off_end) {
            uint64_t back_samples = callback_end_sample - decoded.sample_off_end;
            uint64_t back_ns = (back_samples * 1000000000ULL) / ALSA_CAPTURE_RATE;
            frame_end_est_ns = (ingest_ns > back_ns) ? (ingest_ns - back_ns) : 0;
        }
        if (callback_end_sample >= decoded.sample_off_start) {
            uint64_t back_samples = callback_end_sample - decoded.sample_off_start;
            uint64_t back_ns = (back_samples * 1000000000ULL) / ALSA_CAPTURE_RATE;
            frame_start_est_ns = (ingest_ns > back_ns) ? (ingest_ns - back_ns) : 0;
        }

        /* Mutex held only for the tiny shared-result copy (~<1 µs). */
        pthread_mutex_lock(ctx->shared_mutex);
        ctx->shared->frame     = decoded;
        ctx->shared->fresh     = 1;
        ctx->shared->last_seen = time(NULL);
        ctx->shared->ingest_mono_ns = ingest_ns;
        ctx->shared->frame_mono_ns = mono_ns;
        ctx->shared->frame_start_est_ns = frame_start_est_ns;
        ctx->shared->frame_end_est_ns = frame_end_est_ns;
        pthread_mutex_unlock(ctx->shared_mutex);
    }
    return 0;
}

/* Enumerate RtAudio ALSA devices and return the HiFiBerry input id.
 * Falls back to the ALSA default input if not found by name. */
static unsigned int rta_find_hifiberry_input(rtaudio_t rta)
{
    int ndev = rtaudio_device_count(rta);
    printf("[RTA] RtAudio device count: %d\n", ndev);
    unsigned int fallback = UINT_MAX;
    for (int i = 0; i < ndev; i++) {
        unsigned int did = rtaudio_get_device_id(rta, i);
        rtaudio_device_info_t info = rtaudio_get_device_info(rta, did);
        printf("[RTA] dev %d => id=%u name='%s' inputs=%u outputs=%u\n",
               i, did, info.name, info.input_channels, info.output_channels);
        if (info.input_channels == 0) continue;
        /* Match the ALSA card-id fragment present in both pre- and post-reboot
         * card numbering (sndrpihifiberry / HiFiBerry). */
        if (strstr(info.name, "hifiberry") || strstr(info.name, "HiFiBerry") ||
            strstr(info.name, "sndrpi")) {
            printf("[RTA] HiFiBerry input: %s (id=%u, %u ch)\n",
                   info.name, did, info.input_channels);
            return did;
        }
        if (fallback == UINT_MAX) fallback = did;
    }
    unsigned int def = rtaudio_get_default_input_device(rta);
    if (def != UINT_MAX) {
        rtaudio_device_info_t def_info = rtaudio_get_device_info(rta, def);
        if (def_info.input_channels > 0) {
            printf("[RTA] Using default input device: %s (id=%u, %u ch)\n",
                   def_info.name, def, def_info.input_channels);
            return def;
        }
    }
    if (fallback != UINT_MAX) {
        rtaudio_device_info_t fb_info = rtaudio_get_device_info(rta, fallback);
        printf("[RTA] Using fallback input device: %s (id=%u, %u ch)\n",
               fb_info.name, fallback, fb_info.input_channels);
        return fallback;
    }
    printf("[RTA] No valid ALSA input device found\n");
    return UINT_MAX;
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

static int dump_render_buffer_bmp(const char *path,
                                  const uint8_t *fb,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t pitch)
{
    if (!path || !fb || width == 0 || height == 0 || pitch < width * 4u) {
        return -1;
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        return -1;
    }

    uint32_t row_bytes = width * 3u;
    uint32_t row_padded = (row_bytes + 3u) & ~3u;
    uint32_t image_size = row_padded * height;
    uint32_t file_size = 14u + 40u + image_size;

    unsigned char file_header[14] = {
        'B', 'M',
        (unsigned char)(file_size & 0xFFu),
        (unsigned char)((file_size >> 8) & 0xFFu),
        (unsigned char)((file_size >> 16) & 0xFFu),
        (unsigned char)((file_size >> 24) & 0xFFu),
        0, 0, 0, 0,
        54, 0, 0, 0
    };

    int32_t neg_height = -(int32_t)height; /* top-down bitmap */
    unsigned char dib_header[40] = {
        40, 0, 0, 0,
        (unsigned char)(width & 0xFFu),
        (unsigned char)((width >> 8) & 0xFFu),
        (unsigned char)((width >> 16) & 0xFFu),
        (unsigned char)((width >> 24) & 0xFFu),
        (unsigned char)(neg_height & 0xFF),
        (unsigned char)((neg_height >> 8) & 0xFF),
        (unsigned char)((neg_height >> 16) & 0xFF),
        (unsigned char)((neg_height >> 24) & 0xFF),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        (unsigned char)(image_size & 0xFFu),
        (unsigned char)((image_size >> 8) & 0xFFu),
        (unsigned char)((image_size >> 16) & 0xFFu),
        (unsigned char)((image_size >> 24) & 0xFFu),
        0x13, 0x0B, 0, 0,
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0
    };

    if (fwrite(file_header, 1, sizeof(file_header), out) != sizeof(file_header) ||
        fwrite(dib_header, 1, sizeof(dib_header), out) != sizeof(dib_header)) {
        fclose(out);
        return -1;
    }

    uint8_t *row = malloc(row_padded);
    if (!row) {
        fclose(out);
        return -1;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = fb + (size_t)y * (size_t)pitch;
        for (uint32_t x = 0; x < width; x++) {
            /* Render buffer is XRGB8888 in memory order B,G,R,X on little-endian. */
            row[x * 3u + 0u] = src[x * 4u + 0u];
            row[x * 3u + 1u] = src[x * 4u + 1u];
            row[x * 3u + 2u] = src[x * 4u + 2u];
        }
        for (uint32_t p = row_bytes; p < row_padded; p++) {
            row[p] = 0;
        }
        if (fwrite(row, 1, row_padded, out) != row_padded) {
            free(row);
            fclose(out);
            return -1;
        }
    }

    free(row);
    fclose(out);
    return 0;
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
    /* Diagnostic: proves the signal was actually delivered/handled (vs.
     * the render loop already being stuck in an uninterruptible call at
     * delivery time) and pins the exact wall-clock moment, independent of
     * whatever the main thread is doing. write() is async-signal-safe;
     * printf/fprintf are not, so this can't use the logging used elsewhere
     * in this file. Diagnostic only — not the shutdown-stall fix itself. */
    static const char msg[] = "[MAIN] signal handler entered\n";
    ssize_t unused_rc = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)unused_rc;

    /* Second signal = the graceful path is stuck (or starved by a runaway
     * RT thread) — bail out hard so the process is always killable from
     * the keyboard. DRM keeps scanning out the last frame regardless. */
    if (should_exit) {
        _exit(130);
    }
    should_exit = 1;
}

static inline void timecode_fade_alphas(uint64_t elapsed_ns, uint64_t duration_ns,
                                        uint32_t *alpha_from, uint32_t *alpha_to)
{
    if (duration_ns == 0 || elapsed_ns >= duration_ns) {
        *alpha_to = 255u;
        *alpha_from = 0u;
        return;
    }

    double t = (double)elapsed_ns / (double)duration_ns;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }

    double fade_in = t * t;
    double fade_out = (1.0 - t) * (1.0 - t);
    *alpha_to = (uint32_t)(fade_in * 255.0 + 0.5);
    *alpha_from = (uint32_t)(fade_out * 255.0 + 0.5);
}

/* Computes glyph scale and text/background layout for one output's native
 * fb_width x fb_height (portrait or landscape, whichever the connector's
 * mode reports) and fills a gbm_render_state_t ready to render. All the
 * frame *content* (timecode string, colors, fade state, overlay) is shared
 * across every output and is passed in as-is; only geometry is recomputed
 * per output, since DSI (portrait) and HDMI (landscape) need independently
 * scaled/positioned layouts from the same logical content. */
static void build_output_render_state(gbm_render_state_t *out_state,
                                      uint32_t fb_width, uint32_t fb_height,
                                      uint32_t digit_w, uint32_t glyph_h,
                                      int x_offset, int y_offset,
                                      const char *timecode_str,
                                      uint32_t target_text_color,
                                      uint32_t bg_color,
                                      int source_fade_active,
                                      const char *fade_from_str, uint32_t fade_from_color,
                                      const char *fade_to_str, uint32_t fade_to_color,
                                      int debug_overlay_enabled,
                                      const char *overlay_str,
                                      uint32_t overlay_color,
                                      int df_flag)
{
    int portrait_mode = (fb_height > fb_width);
    uint32_t render_width = portrait_mode ? fb_height : fb_width;
    uint32_t render_height = portrait_mode ? fb_width : fb_height;

    /* The atlas is rasterized at this output's native glyph size (the
     * TIMECODE_FONT_HEIGHT target pre-clamped to fit this mode), so the
     * draw scale is 1.0. The clamp below only guards the transient where
     * mode and atlas disagree (e.g. mid mode-change) — it can only shrink,
     * never enlarge, so glyphs stay unresampled in the steady state. */
    if (digit_w == 0 || glyph_h == 0) {
        digit_w = 1;
        glyph_h = 1;
    }
    float glyph_scale = 1.0f;
    float max_scale_h = (float)render_height / (float)glyph_h;
    float max_scale_bg = (float)render_width / (float)(8 * digit_w);
    float max_safe_scale = (max_scale_h < max_scale_bg) ? max_scale_h : max_scale_bg;
    if (glyph_scale > max_safe_scale) {
        glyph_scale = max_safe_scale;
    }
    if (glyph_scale <= 0.0f) {
        glyph_scale = 1.0f;
    }

    uint32_t scaled_glyph_height = (uint32_t)(glyph_h * glyph_scale + 0.5f);
    int32_t center_y = (render_height > scaled_glyph_height) ?
                       (int32_t)((render_height - scaled_glyph_height) / 2) : 0;
    int32_t text_y_final = center_y + y_offset;
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

    uint32_t base_string_width = 8 * digit_w;
    uint32_t scaled_string_width = (uint32_t)(base_string_width * glyph_scale + 0.5f);
    uint32_t scaled_digit_width = (uint32_t)(digit_w * glyph_scale + 0.5f);
    if (scaled_digit_width == 0) {
        scaled_digit_width = 1;
    }

    /* The 2nd '.' in HH.MM.SS.FF is the natural reading center of the
     * readout. Periods have zero advance width (drawn overlaid on the
     * preceding digit, see gbm_backend_draw_text), so the 2nd period is
     * drawn 3 digit-widths in from the string start — but it marks the
     * *boundary* between MM and SS, i.e. the true midpoint of the 8-digit
     * block, which is 4 digit-widths in (half of 8). Anchoring on the
     * glyph's literal draw position (3 digits) instead of that boundary (4
     * digits) shifts the whole string a full digit-width right of center;
     * this anchors on the boundary, which is exactly plain bounding-box
     * centering of the 8-digit string. */
    int32_t center_x = (render_width > scaled_string_width) ?
                       (int32_t)((render_width - scaled_string_width) / 2) : 0;
    int32_t text_x_final = center_x + x_offset;

    int32_t min_text_x = 0;
    int32_t max_text_x = (int32_t)render_width - (int32_t)scaled_string_width;
    if (max_text_x < 0) {
        max_text_x = 0;
    }
    if (text_x_final < min_text_x) {
        text_x_final = min_text_x;
    }
    if (text_x_final > max_text_x) {
        text_x_final = max_text_x;
    }

    /* Background "88.88.88.88." shares the same 8 digit-slots as the lit
     * text — its only difference is a purely decorative trailing 4th
     * period (a real 7-segment display's unlit extra decimal point), which
     * has zero width and doesn't affect layout — so it starts at the same
     * x as the text rather than offset by a digit. */
    uint32_t bg_x = (uint32_t)text_x_final;

    float overlay_scale = glyph_scale * 0.23f;
    if (overlay_scale < 0.08f) {
        overlay_scale = 0.08f;
    }

    memset(out_state, 0, sizeof(*out_state));
    out_state->portrait_mode = portrait_mode;
    out_state->fb_width = fb_width;
    out_state->fb_height = fb_height;
    out_state->logical_width = render_width;
    out_state->logical_height = render_height;
    out_state->bg_color = bg_color;
    out_state->bg_x = bg_x;
    out_state->text_x = (uint32_t)text_x_final;
    out_state->text_y = (uint32_t)text_y_final;
    out_state->glyph_scale = glyph_scale;
    out_state->timecode_str = timecode_str;
    out_state->target_text_color = target_text_color;
    out_state->source_fade_active = source_fade_active;
    out_state->fade_from_str = fade_from_str;
    out_state->fade_from_color = fade_from_color;
    out_state->fade_to_str = fade_to_str;
    out_state->fade_to_color = fade_to_color;
    out_state->debug_overlay_enabled = debug_overlay_enabled;
    out_state->overlay_str = overlay_str;
    out_state->overlay_color = overlay_color;
    out_state->overlay_scale = overlay_scale;
    out_state->df_flag = df_flag;
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

    /* LTC_NO_RT=1: diagnostic mode — run everything as normal CFS tasks on
     * all cores (no FIFO/RR, no CPU pinning, no RT audio thread). A runaway
     * loop then costs one core's worth of CFS time instead of starving the
     * isolated RT core and tripping the hardware watchdog, and gdb can
     * attach safely. Latency figures are meaningless in this mode. */
    const char *no_rt_env = getenv("LTC_NO_RT");
    int no_rt = (no_rt_env && no_rt_env[0] == '1');
    if (no_rt) {
        printf("[MAIN] LTC_NO_RT=1: RT scheduling and CPU pinning DISABLED (diagnostic mode)\n");
    }

    /* Pin main thread to isolated core 3 when available. */
    if (!no_rt) {
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
    if (no_rt) {
        /* diagnostic mode: stay SCHED_OTHER */
    } else if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
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


    /* Initialize display backend for framebuffer rendering with config refresh rate */
    float target_hz = (current_config.refresh_hz > 0.0f) ? current_config.refresh_hz : (float)TARGET_REFRESH_HZ;
    if (display_backend_init(&g_backend, (uint32_t)current_config.display_width, (uint32_t)current_config.display_height, target_hz)) {
        fprintf(stderr, "Failed to initialize display backend\n");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_BOOTTIME, &ts);
    printf("[MAIN] Display backend initialized: %.2f Hz (boot+%.3f s)\n", display_backend_refresh_hz(&g_backend), ts.tv_sec + ts.tv_nsec/1e9);

    /* Update current_config to reflect what backend actually selected */
    current_config.display_width = display_backend_back_width(&g_backend);
    current_config.display_height = display_backend_back_height(&g_backend);
    current_config.refresh_hz = display_backend_refresh_hz(&g_backend);

    /* Initialize bitmap font */

    if (font_init()) {
        fprintf(stderr, "Failed to initialize font\n");
        display_backend_cleanup(&g_backend);
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
        display_backend_cleanup(&g_backend);
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
        display_backend_cleanup(&g_backend);
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
        display_backend_cleanup(&g_backend);
        return EXIT_FAILURE;
    }

    unsigned int rta_dev = rta_find_hifiberry_input(rtactx.rta);
    if (rta_dev == UINT_MAX) {
        fprintf(stderr, "[RTA] No valid input device available\n");
        rtaudio_destroy(rtactx.rta);
        pthread_mutex_destroy(&ltc_mutex);
        ltc_decoder_cleanup(&ltc);
        display_backend_cleanup(&g_backend);
        return EXIT_FAILURE;
    }

    rtaudio_stream_parameters_t rta_in;
    memset(&rta_in, 0, sizeof(rta_in));
    rta_in.device_id     = rta_dev;
    rta_in.num_channels  = 1;   /* mono — RtAudio extracts channel 0 from hardware */
    rta_in.first_channel = 0;

    g_gpu_clock_thread_running = 1;
    if (pthread_create(&g_gpu_clock_thread, NULL, gpu_clock_reader_thread, NULL) != 0) {
        g_gpu_clock_thread_running = 0;
        fprintf(stderr, "[MAIN] Warning: Failed to start GPU clock reader thread\n");
        fflush(stderr);
    }

    unsigned int requested_rta_buf = read_env_u32("LTC_RTA_PERIOD_FRAMES",
                                                  RTAUDIO_CAPTURE_PERIOD_FRAMES,
                                                  16,
                                                  2048);
    unsigned int rta_buf = requested_rta_buf;
    unsigned int requested_rta_num_buffers = read_env_u32("LTC_RTA_NUM_BUFFERS", 4, 2, 8);
    unsigned int rta_rt_priority = read_env_u32("LTC_RTA_RT_PRIORITY", 80, 50, 98);
    int ltc_smooth_enable = (int)read_env_u32("LTC_SMOOTH_ENABLE", 1, 0, 1);
    unsigned int ltc_phase_advance_frames = read_env_u32("LTC_PHASE_ADVANCE_FRAMES", 0, 0, 4);
    unsigned int ltc_live_need_render_div = read_env_u32("LTC_LIVE_NEED_RENDER_DIV", 20, 2, 128);
    unsigned int ltc_live_idle_div = read_env_u32("LTC_LIVE_IDLE_DIV", 16, 2, 128);
    uint64_t ltc_live_sleep_min_ns = (uint64_t)read_env_u32("LTC_LIVE_SLEEP_MIN_US", 100, 50, 5000) * 1000ULL;
    uint64_t ltc_live_sleep_max_ns = (uint64_t)read_env_u32("LTC_LIVE_SLEEP_MAX_US", 2000, 100, 10000) * 1000ULL;
    unsigned int fade_test_period_ms = read_env_u32("LTC_FADE_TEST_PERIOD_MS", 0, 0, 60000);
    g_log_verbose = (int)read_env_u32("LTC_LOG_VERBOSE", 0, 0, 1);
    if (g_log_verbose) {
        printf("[MAIN] LTC_LOG_VERBOSE=1: logging every rendered/decoded frame (noisy — for debugging only)\n");
    }

    rtaudio_stream_options_t rta_opts;
    memset(&rta_opts, 0, sizeof(rta_opts));
    rta_opts.flags       = RTAUDIO_FLAGS_MINIMIZE_LATENCY;
    if (!no_rt) {
        rta_opts.flags |= RTAUDIO_FLAGS_SCHEDULE_REALTIME;
    }
    rta_opts.num_buffers = requested_rta_num_buffers;
    rta_opts.priority    = (int)rta_rt_priority;
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
        display_backend_cleanup(&g_backend);
        return EXIT_FAILURE;
    }
    rtaudio_start_stream(rtactx.rta);

    clock_gettime(CLOCK_BOOTTIME, &ts);
    fflush(stdout);
        printf("[RTA] Capture started: device=%u req_buf=%u actual_buf=%u frames num_buffers=%u rt_prio=%u S16 mono 48kHz smooth=%s phase_adv=%u"
            " live_sleep[need_div=%u idle_div=%u min_us=%" PRIu64 " max_us=%" PRIu64 "] (boot+%.3f s)\n",
           rta_dev,
           requested_rta_buf,
           rta_buf,
           rta_opts.num_buffers,
            rta_rt_priority,
           ltc_smooth_enable ? "on" : "off",
           ltc_phase_advance_frames,
           ltc_live_need_render_div,
           ltc_live_idle_div,
           (uint64_t)(ltc_live_sleep_min_ns / 1000ULL),
           (uint64_t)(ltc_live_sleep_max_ns / 1000ULL),
           ts.tv_sec + ts.tv_nsec/1e9);

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
    char displayed_timecode_str[32] = {0};
    uint32_t displayed_text_color = 0;
    int have_displayed_timecode = 0;
    int source_fade_active = 0;
    uint64_t source_fade_start_ns = 0;
    char fade_from_str[32] = {0};
    uint32_t fade_from_color = 0;
    char fade_to_str[32] = {0};
    uint32_t fade_to_color = 0;

    uint64_t render_op_count = 0;   /* renders performed in current 5-s window */
    uint64_t skip_count = 0;        /* frames skipped (identical timecode) */
    uint64_t total_render_ns = 0;   /* accumulated render time (ns) */
    uint64_t total_latency_us = 0;  /* accumulated decode->display latency (us) */
    uint64_t latency_samples = 0;   /* decode->display samples in current 5-s window */
    uint64_t total_ingest_to_decode_us = 0;   /* accumulated ADC ingest->decode latency (us) */
    uint64_t ingest_to_decode_samples = 0;
    uint64_t total_ingest_to_display_us = 0;  /* accumulated ADC ingest->display latency (us) */
    uint64_t ingest_to_display_samples = 0;
    uint64_t total_frame_start_to_display_us = 0; /* estimated frame-start->display latency */
    uint64_t frame_start_to_display_samples = 0;
    uint64_t total_frame_end_to_display_us = 0;   /* estimated frame-end->display latency */
    uint64_t frame_end_to_display_samples = 0;
    uint64_t last_render_mono_ns = 0;
    ltc_frame_t last_ltc_frame = {0};
    ltc_frame_t target_ltc_frame = {0};
    uint64_t target_ltc_ingest_ns = 0;
    uint64_t target_ltc_mono_ns = 0;
    uint64_t target_ltc_start_est_ns = 0;
    uint64_t target_ltc_end_est_ns = 0;
    int has_ltc_frame = 0;
    int has_target_ltc_frame = 0;
    int prev_display_ltc_active = 0;
    const char *framegrab_dir = getenv("LTC_FRAMEGRAB_DIR");
    int framegrab_ltc_done = 0;
    int framegrab_tod_done = 0;
    time_t last_ltc_seen = 0;
    struct timespec last_smooth_ts = {0};
    int smooth_clock_init = 0;
    double smooth_budget_frames = 0.0;
    uint64_t last_hotplug_poll_mono_ns = 0;

    clock_gettime(CLOCK_BOOTTIME, &ts);
    fflush(stdout);
    printf("[MAIN] Starting render loop at boot+%.3f s\n", ts.tv_sec + ts.tv_nsec/1e9);
    if (fade_test_period_ms > 0) {
        printf("[MAIN] Fade test mode enabled: toggling LTC/ToD every %u ms\n",
               fade_test_period_ms);
        fflush(stdout);
    }

    while (!should_exit) {
        /* Rescan for HDMI connect/disconnect roughly once a second — no
         * faster, since a cable can't physically change state quicker than
         * that and the DRM ioctls involved aren't worth doing every frame. */
        {
            struct timespec ts_hotplug;
            clock_gettime(CLOCK_MONOTONIC, &ts_hotplug);
            uint64_t now_mono_ns = (uint64_t)ts_hotplug.tv_sec * 1000000000ULL + (uint64_t)ts_hotplug.tv_nsec;
            if (now_mono_ns - last_hotplug_poll_mono_ns >= 1000000000ULL) {
                last_hotplug_poll_mono_ns = now_mono_ns;
                display_backend_poll_hotplug(&g_backend);
            }
        }

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
                                        display_backend_cleanup(&g_backend);
                    float new_target_hz = new_config.refresh_hz;
                                            if (display_backend_init(&g_backend, (uint32_t)new_config.display_width, (uint32_t)new_config.display_height, new_target_hz) == 0) {
                       printf("[MAIN] Display backend reinitialized successfully: %ux%u @ %.2f Hz\n",
                                                     display_backend_back_width(&g_backend), display_backend_back_height(&g_backend), display_backend_refresh_hz(&g_backend));
                        fflush(stdout);
                        /* Update new_config to reflect what backend actually selected */
                                                new_config.display_width = display_backend_back_width(&g_backend);
                                                new_config.display_height = display_backend_back_height(&g_backend);
                                                new_config.refresh_hz = display_backend_refresh_hz(&g_backend);
                    } else {
                       fprintf(stderr, "[MAIN] Failed to reinitialize DRM with %dx%d@%.2f, exiting\n",
                            new_config.display_width, new_config.display_height, new_config.refresh_hz);
                        fflush(stderr);
                        should_exit = 1;
                    }
                }
                current_config = new_config;
                  printf("[MAIN] Config reloaded: TZ=%s, NTP=%s, Mode=%dx%d@%.2f, Color=(%d,%d,%d)\n",
                      current_config.timezone, current_config.ntp_server,
                      current_config.display_width, current_config.display_height, current_config.refresh_hz,
                       current_config.color_r, current_config.color_g, current_config.color_b);
                fflush(stdout);
            }
        }

        /* Get back buffer and render */
        uint8_t *back_buffer = display_backend_get_back_buffer(&g_backend);
        if (!back_buffer) {
            fprintf(stderr, "Failed to get back buffer\n");
            break;
        }

        uint32_t fb_width = display_backend_back_width(&g_backend);
        uint32_t fb_height = display_backend_back_height(&g_backend);
        int portrait_mode = (fb_height > fb_width);

        /*
         * For portrait-only DSI modes (e.g. 480x1920), render into a logical
         * landscape surface and rotate into the real framebuffer.
         */
        uint32_t render_width = portrait_mode ? fb_height : fb_width;
        uint32_t render_height = portrait_mode ? fb_width : fb_height;
        uint32_t render_pitch = display_backend_back_pitch(&g_backend);
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
        float max_scale_bg = (float)render_width / (float)(8 * font_digit_advance());
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
        uint64_t live_ltc_start_est_ns = 0;
        uint64_t live_ltc_end_est_ns = 0;
        int got_ltc = 0;
        pthread_mutex_lock(&ltc_mutex);
        if (ltc_shared.fresh) {
            live_ltc_frame = ltc_shared.frame;
            live_ltc_ingest_ns = ltc_shared.ingest_mono_ns;
            live_ltc_mono_ns = ltc_shared.frame_mono_ns;
            live_ltc_start_est_ns = ltc_shared.frame_start_est_ns;
            live_ltc_end_est_ns = ltc_shared.frame_end_est_ns;
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
            if (ltc_phase_advance_frames > 0) {
                uint32_t fps_for_phase = nominal_ltc_fps ? nominal_ltc_fps : LTC_FRAME_RATE;
                target_ltc_frame = tc_add_steps(&live_ltc_frame, fps_for_phase, ltc_phase_advance_frames);
                target_ltc_frame.drop_frame = live_ltc_frame.drop_frame;
            }
            target_ltc_ingest_ns = live_ltc_ingest_ns;
            target_ltc_mono_ns = live_ltc_mono_ns;
            target_ltc_start_est_ns = live_ltc_start_est_ns;
            target_ltc_end_est_ns = live_ltc_end_est_ns;
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
                target_ltc_start_est_ns = 0;
                target_ltc_end_est_ns = 0;
                smooth_budget_frames = 0.0;
                ltc_frame_to_string_with_tz(&last_ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
            } else {
                if (has_target_ltc_frame && got_ltc) {
                    if (!ltc_smooth_enable) {
                        last_ltc_frame = target_ltc_frame;
                        smooth_budget_frames = 0.0;
                    } else {
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
                }
                ltc_frame_to_string(&last_ltc_frame, timecode_str, sizeof(timecode_str));
            }
        } else {
            ltc_frame_to_string_with_tz(&last_ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
        }

        struct timespec ts_loop_mono;
        clock_gettime(CLOCK_MONOTONIC, &ts_loop_mono);
        uint64_t loop_mono_ns = (uint64_t)ts_loop_mono.tv_sec * 1000000000ULL + (uint64_t)ts_loop_mono.tv_nsec;

        /* Consider LTC live for one second after the latest decoded frame. */
        int ltc_live = has_ltc_frame && ((now - last_ltc_seen) <= 1);
        /* Display source remains LTC until configured loss timeout expires. */
        int display_ltc_active = has_ltc_frame;

        if (fade_test_period_ms > 0) {
            uint64_t fade_test_period_ns = (uint64_t)fade_test_period_ms * 1000000ULL;
            int fade_test_ltc_active = ((loop_mono_ns / fade_test_period_ns) & 1ULL) != 0;
            if (fade_test_ltc_active) {
                struct timespec ts_wall;
                clock_gettime(CLOCK_REALTIME, &ts_wall);
                unsigned int fade_test_frame = (unsigned int)((ts_wall.tv_nsec * LTC_FRAME_RATE) / 1000000000ULL);
                if (fade_test_frame >= LTC_FRAME_RATE) {
                    fade_test_frame = LTC_FRAME_RATE - 1;
                }
                snprintf(timecode_str, sizeof(timecode_str), "%02d.%02d.%02d.%02u",
                         tm_local->tm_hour, tm_local->tm_min, tm_local->tm_sec, fade_test_frame);
                ltc_live = 1;
                display_ltc_active = 1;
            } else {
                ltc_frame_to_string_with_tz(&last_ltc_frame, tm_local, timecode_str, sizeof(timecode_str));
                ltc_live = 0;
                display_ltc_active = 0;
            }
        }

        uint32_t target_text_color;
        if (display_ltc_active) {
            target_text_color = 0x00FF0000;
        } else {
            target_text_color = ((current_config.color_r << 16) |
                                 (current_config.color_g << 8) |
                                 (current_config.color_b));
        }

        /* Hybrid render gate:
         * - Live LTC: redraw only on fresh decoded frames and visible text changes.
         * - No live LTC: redraw on ToD text changes so fallback remains active.
         * - Always redraw on LTC live/loss transitions for immediate UX feedback.
         * - For GBM/EGL, keep the GPU path active by rendering every frame. */
        int text_changed = (strcmp(timecode_str, prev_timecode_str) != 0);
        int display_state_changed = (display_ltc_active != prev_display_ltc_active);

        if (display_state_changed && have_displayed_timecode) {
            memcpy(fade_from_str, displayed_timecode_str, sizeof(fade_from_str));
            fade_from_color = displayed_text_color;
            memcpy(fade_to_str, timecode_str, sizeof(fade_to_str));
            fade_to_color = target_text_color;
            source_fade_active = 1;
            source_fade_start_ns = loop_mono_ns;
        }

        int need_render = (g_backend.type == DISPLAY_BACKEND_GBM) ? 1 :
                          ((display_ltc_active ? (got_ltc && text_changed) : text_changed) ||
                           display_state_changed || source_fade_active);
        prev_display_ltc_active = display_ltc_active;
        int immediate_ltc_update = (display_ltc_active && got_ltc && text_changed);
        if (need_render && !source_fade_active && !immediate_ltc_update &&
            g_backend.type != DISPLAY_BACKEND_GBM) {
            uint64_t now_mono_ns = loop_mono_ns;

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
            int32_t text_y_final = center_y + 0; /* legacy software-raster path: DSI is locked to center */
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
            uint32_t base_string_width = 8 * font_digit_advance();
            uint32_t scaled_string_width = (uint32_t)(base_string_width * glyph_scale + 0.5f);

            /* Calculate scaled digit width for background bounds */
            uint32_t scaled_digit_width = (uint32_t)(font_digit_advance() * glyph_scale + 0.5f);
            if (scaled_digit_width == 0) {
                scaled_digit_width = 1;
            }

            /* Center timecode, then clamp so full background stays on screen */
            int32_t center_x = (render_width > scaled_string_width) ?
                               (int32_t)((render_width - scaled_string_width) / 2) : 0;
            int32_t text_x_final = center_x + 0; /* legacy software-raster path: DSI is locked to center */

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

            char overlay_str[32] = {0};
            unsigned int df_flag = (ltc_live && last_ltc_frame.drop_frame) ? 1u : 0u;
            float overlay_scale = glyph_scale * 0.23f;
            if (overlay_scale < 0.08f) {
                overlay_scale = 0.08f;
            }
            uint32_t expected_fps = LTC_FRAME_RATE;
            uint32_t overlay_color = (nominal_ltc_fps != 0 && nominal_ltc_fps != expected_fps)
                ? 0x00FF8800 : 0x00FFFF00;
            if (current_config.debug_overlay_enabled) {
                int fps_int = (int)detected_ltc_fps;
                int fps_frac = (int)llround((detected_ltc_fps - (float)fps_int) * 100.0f);
                if (fps_frac < 0) {
                    fps_frac = 0;
                }
                if (fps_frac > 99) {
                    fps_frac = 99;
                }
                unsigned int gap_mod = (unsigned int)(ltc_gap_count % 100u);
                snprintf(overlay_str, sizeof(overlay_str), "%02d.%02d.%02u.%02u.%02u",
                         fps_int, fps_frac, nominal_ltc_fps, expected_fps, gap_mod);
            }

            uint64_t source_fade_duration_ns = 0;
            uint64_t render_mono_ns = 0;
            uint64_t elapsed_ns = 0;
            uint32_t from_color = 0;
            uint32_t to_color = 0;
            if (source_fade_active) {
                source_fade_duration_ns = (uint64_t)(current_config.transition_fade_ms > 0
                    ? current_config.transition_fade_ms : 0) * 1000000ULL;
                render_mono_ns = (uint64_t)t_render_start.tv_sec * 1000000000ULL + (uint64_t)t_render_start.tv_nsec;
                elapsed_ns = (render_mono_ns > source_fade_start_ns)
                    ? (render_mono_ns - source_fade_start_ns) : 0;
                uint32_t alpha_from = 0;
                uint32_t alpha_to = 0;
                timecode_fade_alphas(elapsed_ns, source_fade_duration_ns,
                                      &alpha_from, &alpha_to);
                from_color = ((alpha_from & 0xFFu) << 24) | (fade_from_color & 0x00FFFFFFu);
                to_color = ((alpha_to & 0xFFu) << 24) | (fade_to_color & 0x00FFFFFFu);
            }

            if (g_backend.type == DISPLAY_BACKEND_GBM) {
                int primary_failed = 0;
                int output_slots = display_backend_output_count(&g_backend);
                for (int out_idx = 0; out_idx < output_slots; out_idx++) {
                    if (!display_backend_output_active(&g_backend, out_idx)) {
                        continue;
                    }
                    if (display_backend_output_flip_pending(&g_backend, out_idx)) {
                        /* Previous flip hasn't hit vblank yet (slow or
                         * stalled output) — skip this frame for it rather
                         * than block the others. */
                        continue;
                    }

                    uint32_t out_fb_width = display_backend_output_width(&g_backend, out_idx);
                    uint32_t out_fb_height = display_backend_output_height(&g_backend, out_idx);

                    /* DSI (always output 0) is permanently locked to center.
                     * Every HDMI output gets its own saved offset, keyed by
                     * its stable port name (e.g. "HDMI-A-1"), so different
                     * ports can be positioned independently. */
                    int out_x_offset = 0;
                    int out_y_offset = 0;
                    if (out_idx != 0) {
                        char port_name[32];
                        drm_connector_name(display_backend_output_connector_type(&g_backend, out_idx),
                                            display_backend_output_connector_type_id(&g_backend, out_idx),
                                            port_name, sizeof(port_name));
                        config_get_hdmi_offset(&current_config, port_name, &out_x_offset, &out_y_offset);
                    }

                    gbm_render_state_t gbm_state;
                    build_output_render_state(&gbm_state, out_fb_width, out_fb_height,
                                               display_backend_output_digit_width(&g_backend, out_idx),
                                               display_backend_output_glyph_height(&g_backend, out_idx),
                                               out_x_offset, out_y_offset,
                                               timecode_str, target_text_color, bg_color,
                                               source_fade_active,
                                               fade_from_str, from_color,
                                               fade_to_str, to_color,
                                               current_config.debug_overlay_enabled,
                                               overlay_str, overlay_color, (int)df_flag);

                    int render_rc = display_backend_render_output(&g_backend, out_idx, &gbm_state);

                    /* Debug framegrab hook (readback must happen after the
                     * render, before the flip swaps the buffer away). */
                    if (render_rc == 0 && out_idx == 0 &&
                        framegrab_dir && framegrab_dir[0] != '\0' && !source_fade_active &&
                        ((display_ltc_active && !framegrab_ltc_done) ||
                         (!display_ltc_active && !framegrab_tod_done))) {
                        uint8_t *grab_buf = gbm_backend_get_back_buffer(&g_backend.ctx.gbm);
                        if (grab_buf &&
                            gbm_backend_read_pixels(&g_backend.ctx.gbm, out_idx, grab_buf) == 0) {
                            char path[512];
                            snprintf(path, sizeof(path), "%s/%s.bmp", framegrab_dir,
                                     display_ltc_active ? "ltc-live" : "tod-fallback");
                            if (dump_render_buffer_bmp(path, grab_buf,
                                                       out_fb_width, out_fb_height,
                                                       out_fb_width * 4u) == 0) {
                                if (display_ltc_active) framegrab_ltc_done = 1;
                                else framegrab_tod_done = 1;
                                printf("[FRAMEGRAB] Captured %s frame: %s\n",
                                       display_ltc_active ? "LTC" : "ToD", path);
                                fflush(stdout);
                            }
                        }
                    }

                    if (render_rc != 0 ||
                        display_backend_flip_output(&g_backend, out_idx) != 0) {
                        if (out_idx == 0) {
                            fprintf(stderr, "[MAIN] GPU render/flip failed on primary output\n");
                            fflush(stderr);
                            primary_failed = 1;
                            break;
                        }
                        /* A secondary HDMI output failing (e.g. mid-unplug
                         * race lost to the next hotplug poll) must not take
                         * the permanent DSI panel down with it. */
                        fprintf(stderr, "[MAIN] Output %d render/flip failed, deactivating\n", out_idx);
                        fflush(stderr);
                        display_backend_deactivate_output(&g_backend, out_idx);
                    }
                }
                if (primary_failed) {
                    break;
                }

                if (source_fade_active) {
                    if (source_fade_duration_ns == 0 || elapsed_ns >= source_fade_duration_ns) {
                        source_fade_active = 0;
                        memcpy(displayed_timecode_str, fade_to_str, sizeof(displayed_timecode_str));
                        displayed_text_color = fade_to_color;
                        have_displayed_timecode = 1;
                    }
                } else {
                    memcpy(displayed_timecode_str, timecode_str, sizeof(displayed_timecode_str));
                    displayed_text_color = target_text_color;
                    have_displayed_timecode = 1;
                }
            } else {
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
                if (source_fade_active) {
                    uint64_t source_fade_duration_ns = (uint64_t)(current_config.transition_fade_ms > 0
                        ? current_config.transition_fade_ms : 0) * 1000000ULL;
                    uint64_t render_mono_ns = (uint64_t)t_render_start.tv_sec * 1000000000ULL + (uint64_t)t_render_start.tv_nsec;
                    uint64_t elapsed_ns = (render_mono_ns > source_fade_start_ns)
                        ? (render_mono_ns - source_fade_start_ns) : 0;
                    uint32_t alpha_from = 0;
                    uint32_t alpha_to = 0;
                    timecode_fade_alphas(elapsed_ns, source_fade_duration_ns,
                                          &alpha_from, &alpha_to);

                    uint32_t from_color = ((alpha_from & 0xFFu) << 24) | (fade_from_color & 0x00FFFFFFu);
                    uint32_t to_color = ((alpha_to & 0xFFu) << 24) | (fade_to_color & 0x00FFFFFFu);
                    if (alpha_from > 0) {
                        if (portrait_mode) {
                            font_blit_string_scaled_rot90ccw(render_buffer, fb_width, fb_height, render_pitch,
                                                             render_width, render_height,
                                                             text_x, text_y, fade_from_str,
                                                             from_color, glyph_scale);
                        } else {
                            font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                                                    text_x, text_y, fade_from_str,
                                                    from_color, glyph_scale);
                        }
                    }

                    if (alpha_to > 0) {
                        if (portrait_mode) {
                            font_blit_string_scaled_rot90ccw(render_buffer, fb_width, fb_height, render_pitch,
                                                             render_width, render_height,
                                                             text_x, text_y, fade_to_str,
                                                             to_color, glyph_scale);
                        } else {
                            font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                                                    text_x, text_y, fade_to_str,
                                                    to_color, glyph_scale);
                        }
                    }

                    if (source_fade_duration_ns == 0 || elapsed_ns >= source_fade_duration_ns) {
                        source_fade_active = 0;
                        memcpy(displayed_timecode_str, fade_to_str, sizeof(displayed_timecode_str));
                        displayed_text_color = fade_to_color;
                        have_displayed_timecode = 1;
                    }
                } else {
                    if (portrait_mode) {
                        font_blit_string_scaled_rot90ccw(render_buffer, fb_width, fb_height, render_pitch,
                                                         render_width, render_height,
                                                         text_x, text_y, timecode_str,
                                                         target_text_color, glyph_scale);
                    } else {
                        font_blit_string_scaled(render_buffer, render_width, render_height, render_pitch,
                                                text_x, text_y, timecode_str,
                                                target_text_color, glyph_scale);
                    }
                    memcpy(displayed_timecode_str, timecode_str, sizeof(displayed_timecode_str));
                    displayed_text_color = target_text_color;
                    have_displayed_timecode = 1;
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
                        uint32_t df_x = overlay_x + (uint32_t)(16.0f * font_digit_advance() * overlay_scale);
                        draw_df_badge(render_buffer, fb_width, fb_height, render_pitch,
                                      render_width, render_height, portrait_mode,
                                      df_x, overlay_y,
                                      overlay_color, overlay_scale * 0.72f);
                    }
                }

                if (framegrab_dir && framegrab_dir[0] != '\0') {
                    if (display_ltc_active && !source_fade_active && !framegrab_ltc_done) {
                        char path[512];
                        snprintf(path, sizeof(path), "%s/ltc-live.bmp", framegrab_dir);
                        if (dump_render_buffer_bmp(path, render_buffer, fb_width, fb_height, render_pitch) == 0) {
                            framegrab_ltc_done = 1;
                            printf("[FRAMEGRAB] Captured LTC frame: %s\n", path);
                        }
                    } else if (!display_ltc_active && !source_fade_active && !framegrab_tod_done) {
                        char path[512];
                        snprintf(path, sizeof(path), "%s/tod-fallback.bmp", framegrab_dir);
                        if (dump_render_buffer_bmp(path, render_buffer, fb_width, fb_height, render_pitch) == 0) {
                            framegrab_tod_done = 1;
                            printf("[FRAMEGRAB] Captured ToD frame: %s\n", path);
                        }
                    }
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

                if (target_ltc_start_est_ns > 0 && now_mono_ns >= target_ltc_start_est_ns) {
                    total_frame_start_to_display_us += (now_mono_ns - target_ltc_start_est_ns) / 1000ULL;
                    frame_start_to_display_samples++;
                }

                if (target_ltc_end_est_ns > 0 && now_mono_ns >= target_ltc_end_est_ns) {
                    total_frame_end_to_display_us += (now_mono_ns - target_ltc_end_est_ns) / 1000ULL;
                    frame_end_to_display_samples++;
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

            /* GBM outputs already flipped per-output above; this generic
             * single-output call only applies to the legacy DRM backend. */
            if (g_backend.type != DISPLAY_BACKEND_GBM &&
                display_backend_page_flip_sync(&g_backend)) {
                fprintf(stderr, "[MAIN] Page flip failed\n");
                fflush(stderr);
                break;
            }
        } else {
            skip_count++;
        }

        frame_count++;

        /* Log status every ~5 seconds */
        if ((frame_count % (5 * (uint32_t)display_backend_refresh_hz(&g_backend))) == 0) {
            uint64_t avg_us = render_op_count > 0
                ? total_render_ns / render_op_count / 1000 : 0;
            uint64_t avg_decode_to_display_us = latency_samples > 0
                ? total_latency_us / latency_samples : 0;
            uint64_t avg_ingest_to_decode_us = ingest_to_decode_samples > 0
                ? total_ingest_to_decode_us / ingest_to_decode_samples : 0;
            uint64_t avg_ingest_to_display_us = ingest_to_display_samples > 0
                ? total_ingest_to_display_us / ingest_to_display_samples : 0;
            uint64_t avg_frame_start_to_display_us = frame_start_to_display_samples > 0
                ? total_frame_start_to_display_us / frame_start_to_display_samples : 0;
            uint64_t avg_frame_end_to_display_us = frame_end_to_display_samples > 0
                ? total_frame_end_to_display_us / frame_end_to_display_samples : 0;
            double avg_in_out_ms = (double)avg_ingest_to_display_us / 1000.0;
            double avg_tc_end_disp_ms = (double)avg_frame_end_to_display_us / 1000.0;
            double avg_tc_start_disp_ms = (double)avg_frame_start_to_display_us / 1000.0;
            double half_scan_ms = (display_backend_refresh_hz(&g_backend) > 0.0f)
                ? (1000.0 / ((double)display_backend_refresh_hz(&g_backend) * 2.0)) : 0.0;
            double avg_tc_end_glass_mid_ms = avg_tc_end_disp_ms + half_scan_ms;
            /* adc_disp_lat_us/tc_start_disp_lat_us/tc_end_disp_lat_us were
             * exact duplicates of in_out_ms/tc_start_disp_ms/tc_end_disp_ms
             * below in different units — dropped. adc_dec_lat_us (ingest to
             * decode) and dec_disp_lat_us (decode to display) are NOT
             * duplicates of anything else here and are kept. Also removed a
             * genuinely unreachable middle branch that re-checked
             * gpu_clock_x10>0 inside its own else (2026-08-02). gpu_mhz is
             * appended only once the clock-reader thread has a first
             * sample. */
            long gpu_clock_x10 = g_gpu_clock_mhz_x10;
            printf("[MAIN] Frame %" PRIu64 ", TC: %s | rendered=%" PRIu64
                   " skipped=%" PRIu64 " avg_render=%" PRIu64 "us ltc_fps=%.2f "
                   "adc_dec_lat_us=%" PRIu64 " dec_disp_lat_us=%" PRIu64
                   " in_out_ms=%.3f tc_end_disp_ms=%.3f tc_start_disp_ms=%.3f tc_end_glass_mid_ms=%.3f",
                   frame_count, timecode_str, render_op_count, skip_count, avg_us, detected_ltc_fps,
                   avg_ingest_to_decode_us, avg_decode_to_display_us,
                   avg_in_out_ms, avg_tc_end_disp_ms, avg_tc_start_disp_ms, avg_tc_end_glass_mid_ms);
            if (gpu_clock_x10 > 0) {
                printf(" gpu_mhz=%.1f", (double)gpu_clock_x10 / 10.0);
            }
            printf(" rta_in_ovf=%" PRIu64 " rta_out_udf=%" PRIu64 "\n",
                   g_rta_input_overflow, g_rta_output_underflow);
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
            total_frame_start_to_display_us = 0;
            frame_start_to_display_samples = 0;
            total_frame_end_to_display_us = 0;
            frame_end_to_display_samples = 0;
        }

        /* Adaptive pacing to avoid busy-spin when content is unchanged. */
        {
            float backend_hz = display_backend_refresh_hz(&g_backend);
            uint64_t target_period_ns;
            if (backend_hz > 0.0f) {
                target_period_ns = (uint64_t)(1000000000.0 / (double)backend_hz);
            } else if (ltc_live) {
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

            if (g_backend.type == DISPLAY_BACKEND_GBM) {
                /* Flip-completion-driven pacing: block until the queued
                 * page flips land on their vblanks (bounded by ~2 frame
                 * periods for a stalled output). Waking on the vblank means
                 * the next iteration renders the freshest LTC frame with a
                 * full refresh period of margin before the next latch. */
                int timeout_ms = (int)((2ULL * target_period_ns) / 1000000ULL);
                if (timeout_ms < 5) timeout_ms = 5;
                int outputs_pending = 0;
                for (int i = 0; i < display_backend_output_count(&g_backend); i++) {
                    if (display_backend_output_flip_pending(&g_backend, i)) {
                        outputs_pending++;
                    }
                }
                if (outputs_pending > 0) {
                    display_backend_wait_flips(&g_backend, timeout_ms);
                } else {
                    /* Nothing in flight (all outputs failed/idle): don't
                     * busy-spin. */
                    struct timespec ts_idle = { 0, 2000000L }; /* 2 ms */
                    nanosleep(&ts_idle, NULL);
                }
                continue;
            }

            uint64_t sleep_ns;
            if (ltc_live) {
                sleep_ns = need_render
                    ? (target_period_ns / (uint64_t)ltc_live_need_render_div)
                    : (target_period_ns / (uint64_t)ltc_live_idle_div);
            } else {
                sleep_ns = need_render ? (target_period_ns / 10ULL) : (target_period_ns / 4ULL);
            }

            /* Keep checks responsive for config updates and LTC lock changes. */
            if (g_backend.type == DISPLAY_BACKEND_GBM) {
                if (sleep_ns < 500000ULL) {
                    sleep_ns = 500000ULL;      /* 0.5 ms min */
                }
                if (sleep_ns > target_period_ns) {
                    sleep_ns = target_period_ns;
                }
            } else if (ltc_live) {
                if (sleep_ns < ltc_live_sleep_min_ns) {
                    sleep_ns = ltc_live_sleep_min_ns;
                }
                if (sleep_ns > ltc_live_sleep_max_ns) {
                    sleep_ns = ltc_live_sleep_max_ns;
                }
            } else {
                if (sleep_ns < 500000ULL) {
                    sleep_ns = 500000ULL;      /* 0.5 ms min */
                }
                if (sleep_ns > 8000000ULL) {
                    sleep_ns = 8000000ULL;     /* 8 ms max */
                }
            }

            struct timespec ts_sleep = {
                .tv_sec = (time_t)(sleep_ns / 1000000000ULL),
                .tv_nsec = (long)(sleep_ns % 1000000000ULL)
            };
            nanosleep(&ts_sleep, NULL);
        }
    }

    printf("[MAIN] Shutting down...\n");

    /* Hard backstop for the ENTIRE shutdown sequence below, spawned before
     * any teardown call runs — see the comment on g_shutdown_done for the
     * root cause this guards against (rtaudio_abort_stream() hanging the
     * whole machine during a real poweroff). g_shutdown_done is set at the
     * very end, right before the final "Cleanup complete" print. */
    {
        pthread_t shutdown_wd_tid;
        if (pthread_create(&shutdown_wd_tid, NULL, shutdown_watchdog_thread, NULL) == 0) {
            pthread_detach(shutdown_wd_tid);
        }
    }

    /* Also drop out of SCHED_FIFO / CPU-3 pinning early: harmless and
     * arguably good hygiene (lets the kernel schedule this thread freely
     * once it's no longer doing latency-sensitive work), even though
     * checkpoint logging on 2026-08-02 showed this was NOT the cause of
     * the shutdown hang — the process was already stuck in
     * rtaudio_abort_stream() before this point, so an RT/RCU-isolation
     * theory considered earlier is ruled out. Kept for the modest
     * scheduling-hygiene benefit, not as a fix. */
    log_shutdown_checkpoint("loop exited, entering shutdown");

    /* Deliberately NOT calling rtaudio_abort_stream/close_stream/destroy
     * here. Checkpoint logging on 2026-08-02 caught this process hanging
     * inside rtaudio_abort_stream() on every single full-`poweroff`
     * attempt, and the 3s shutdown-watchdog backstop above never managed
     * to fire either — meaning the hang is a genuine uninterruptible
     * kernel wait (ALSA driver lock contention with alsa-restore.service,
     * which only runs concurrently during a real multi-service shutdown,
     * never during a standalone `systemctl stop`), not something any
     * signal — SIGKILL from systemd or our own _exit() from another
     * thread — can force through. A thread parked in D-state doesn't
     * respond to _exit()'s exit_group() any more than it responds to
     * SIGKILL. The only reliable fix is to not enter that call at all:
     * skip the graceful ALSA stream teardown and let process exit close
     * the underlying fd. rtactx.rta is intentionally leaked here — the
     * process is terminating regardless. */
    if (g_gpu_clock_thread_running) {
        log_shutdown_checkpoint("gpu_clock_thread join: start");
        g_gpu_clock_thread_running = 0;
        pthread_join(g_gpu_clock_thread, NULL);
        log_shutdown_checkpoint("gpu_clock_thread join: done");
    }
    {
        struct sched_param revert_param = {0};
        sched_setscheduler(0, SCHED_OTHER, &revert_param);
        cpu_set_t all_cpus;
        CPU_ZERO(&all_cpus);
        for (int c = 0; c < CPU_SETSIZE; c++) {
            CPU_SET(c, &all_cpus);
        }
        sched_setaffinity(0, sizeof(all_cpus), &all_cpus);
    }
    log_shutdown_checkpoint("RT priority/affinity dropped");

    /* Blank every active output before tearing down. DRM keeps scanning out
     * whatever framebuffer was last flipped even after this process exits,
     * so on a graceful shutdown (SIGTERM/SIGINT — systemd gives us
     * TimeoutStopSec to do this) the panel would otherwise be left frozen
     * on the last rendered timecode instead of going blank. */
    if (g_backend.type == DISPLAY_BACKEND_GBM) {
        gbm_render_state_t blank_state;
        memset(&blank_state, 0, sizeof(blank_state));
        blank_state.timecode_str = "";
        blank_state.bg_color = 0x000000;

        int blank_slots = display_backend_output_count(&g_backend);
        for (int i = 0; i < blank_slots; i++) {
            if (!display_backend_output_active(&g_backend, i)) {
                continue;
            }
            char ckpt_label[64];
            snprintf(ckpt_label, sizeof(ckpt_label), "output %d render_output: start", i);
            log_shutdown_checkpoint(ckpt_label);
            display_backend_render_output(&g_backend, i, &blank_state);
            snprintf(ckpt_label, sizeof(ckpt_label), "output %d render_output: done, flip_sync: start", i);
            log_shutdown_checkpoint(ckpt_label);
            /* Synchronous present: the blank frame must be on scanout
             * before this process exits, not queued behind a vblank event
             * nobody will be alive to collect. */
            display_backend_flip_output_sync(&g_backend, i);
            snprintf(ckpt_label, sizeof(ckpt_label), "output %d flip_sync: done", i);
            log_shutdown_checkpoint(ckpt_label);
        }
    }
    log_shutdown_checkpoint("blanking loop done, entering final cleanup");

    /* Cleanup (RtAudio stream and GPU clock thread already stopped above,
     * ahead of the display teardown — see the comment there). */
    pthread_mutex_destroy(&ltc_mutex);
    ltc_decoder_cleanup(&ltc);
    free(static_layer_buffer);
    config_watcher_cleanup(&config_watcher);
    log_shutdown_checkpoint("display_backend_cleanup: start");
    display_backend_cleanup(&g_backend);
    log_shutdown_checkpoint("display_backend_cleanup: done");

    g_shutdown_done = 1;
    printf("[MAIN] Cleanup complete. Exiting.\n");
    return EXIT_SUCCESS;
}

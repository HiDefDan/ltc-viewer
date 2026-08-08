#ifndef LTC_TIMECODE_H
#define LTC_TIMECODE_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <ltc.h>

/* LTC timecode frame (24-bit + parity) */
typedef struct {
    uint32_t frame;     /* 0-29 (or 0-24 for 25fps) */
    uint32_t seconds;   /* 0-59 */
    uint32_t minutes;   /* 0-59 */
    uint32_t hours;     /* 0-23 */
    uint32_t days;      /* 0-? for multi-day markers or user bits */
    int drop_frame;     /* 1 when source marks drop-frame timecode */
    uint64_t sample_off_start; /* libltc sample offset for start of decoded frame */
    uint64_t sample_off_end;   /* libltc sample offset for end of decoded frame */
} ltc_frame_t;

/* Shared state written by whichever capture/receiver thread is active
 * (RtAudio/ALSA callback, or the AES67 multicast receiver thread), read by
 * the main render thread. Protected by a mutex that's held only for a
 * memcpy-sized copy — never during DRM renders or libltc decode, so the
 * feeding thread never blocks for more than ~1us. Moved here (was main.c
 * only) so more than one ingest source can share the same publish path via
 * ltc_feed_and_publish(). */
typedef struct {
    ltc_frame_t frame;
    int         fresh;      /* 1 = new frame since last main-thread read */
    time_t      last_seen;
    uint64_t    ingest_mono_ns;
    uint64_t    frame_mono_ns;
    uint64_t    frame_start_est_ns;
    uint64_t    frame_end_est_ns;
} ltc_shared_t;

/* LTC decoder state machine (uses libltc internally) */
typedef struct {
    uint32_t bit_phase;             /* phase counter for bit extraction */
    uint32_t bit_buffer;            /* 80-bit shift register */
    uint32_t bit_count;             /* bits received in current frame */
    ltc_frame_t pending_decoded;
    ltc_frame_t last_decoded;
    uint32_t frame_rate;            /* 24/25/30 fps */
    int valid;                      /* 1 if last frame passed parity check */
    uint32_t lock_streak;           /* consecutive coherent frames while acquiring lock */
    uint64_t sample_pos;            /* monotonic audio sample offset for libltc */
    uint32_t apv_current;           /* audio samples-per-video-frame used by decoder */
    uint32_t apv_mismatch_count;    /* consecutive APV mismatch counter */
    float detected_fps;             /* smoothed detected input LTC rate */
    float detected_fps_inst;        /* latest instantaneous detected LTC rate */
    uint32_t nominal_fps;           /* snapped runtime fps (24/25/30) for validation */
    uint64_t decoded_count;         /* total libltc frames popped from decoder queue */
    uint64_t last_decode_sample;    /* sample_pos at last successfully popped frame */
    uint64_t gap_count;             /* cumulative continuity gaps detected (delta > 1) */
    int gap_reset_armed;            /* 1 after gap-triggered reset until next decode */
    void *libltc;                   /* opaque pointer to LTCDecoder from libltc */
} ltc_decoder_t;

/* Initialize LTC decoder */
int ltc_decoder_init(ltc_decoder_t *decoder, uint32_t frame_rate);

/* Feed ALSA audio samples to decoder.
   samples: mono U8 buffer (already converted from S32 ALSA capture).
   count: number of samples. */
int ltc_feed_audio(ltc_decoder_t *decoder, const int16_t *samples,
                   uint32_t count, int channels);

/* Get last valid decoded frame */
int ltc_get_frame(ltc_decoder_t *decoder, ltc_frame_t *frame);

/* Get smoothed detected LTC input rate in fps (0.0f if unknown). */
float ltc_get_detected_fps(const ltc_decoder_t *decoder);

/* Get current nominal fps used for continuity checks (24/25/30). */
uint32_t ltc_get_nominal_fps(const ltc_decoder_t *decoder);

/* Get cumulative continuity gap events (delta > 1 frame). */
uint64_t ltc_get_gap_count(const ltc_decoder_t *decoder);

/* Feed a batch of mono int16 samples to the decoder, drain every LTC frame
 * it produces, and publish each one to *shared under shared_mutex — the
 * exact feed+drain+publish sequence the RtAudio/ALSA callback used to do
 * inline, factored out so the AES67 receiver thread (src/aes67.c) can do
 * byte-for-byte the same thing regardless of where the samples came from.
 * ingest_ns is CLOCK_MONOTONIC at the moment this batch was received, used
 * to back-date each decoded frame's start/end timestamp estimate from its
 * position within the batch. Defined in main.c (needs no main.c-only
 * state besides the nominal sample rate, already project-wide in
 * config.h) — not moved to ltc.c since it's about *publishing* decoded
 * output, not decoding itself. */
void ltc_feed_and_publish(ltc_decoder_t *decoder, ltc_shared_t *shared,
                          pthread_mutex_t *shared_mutex,
                          const int16_t *pcm, unsigned int nframes,
                          uint64_t ingest_ns);

/* Cleanup decoder resources */
void ltc_decoder_cleanup(ltc_decoder_t *decoder);

/* Convert LTC frame to human-readable string "HH:MM:SS:FF" */
void ltc_frame_to_string(const ltc_frame_t *frame, char *buf, size_t buflen);

/* Convert LTC frame + system time to HH:MM:SS:FF with timezone support */
void ltc_frame_to_string_with_tz(const ltc_frame_t *frame, const struct tm *tm_local,
                                 char *buf, size_t buflen);

#endif /* LTC_TIMECODE_H */

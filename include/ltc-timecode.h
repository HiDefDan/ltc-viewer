#ifndef LTC_TIMECODE_H
#define LTC_TIMECODE_H

#include <stdint.h>
#include <time.h>

/* LTC timecode frame (24-bit + parity) */
typedef struct {
    uint32_t frame;     /* 0-29 (or 0-24 for 25fps) */
    uint32_t seconds;   /* 0-59 */
    uint32_t minutes;   /* 0-59 */
    uint32_t hours;     /* 0-23 */
    uint32_t days;      /* 0-? for multi-day markers or user bits */
} ltc_frame_t;

/* LTC decoder state machine (uses libltc internally) */
typedef struct {
    uint32_t bit_phase;             /* phase counter for bit extraction */
    uint32_t bit_buffer;            /* 80-bit shift register */
    uint32_t bit_count;             /* bits received in current frame */
    uint32_t last_edge_time_us;     /* last GPIO edge timestamp (µs) */
    ltc_frame_t last_decoded;
    uint32_t frame_rate;            /* 24/25/30 fps */
    int valid;                      /* 1 if last frame passed parity check */
    void *libltc;                   /* opaque pointer to LTCDecoder from libltc */
} ltc_decoder_t;

/* Initialize LTC decoder */
int ltc_decoder_init(ltc_decoder_t *decoder, uint32_t frame_rate);

/* Feed ALSA audio samples to decoder
   samples: interleaved audio buffer (int16_t)
   count: number of samples per channel
   channels: 1 (mono) or 2 (stereo) */
int ltc_feed_audio(ltc_decoder_t *decoder, const int16_t *samples, 
                   uint32_t count, int channels);

/* Feed GPIO edge (edge_time_us from DMA timestamp) to decoder (deprecated, use ltc_feed_audio) */
int ltc_feed_edge(ltc_decoder_t *decoder, uint64_t edge_time_us, int level);

/* Get last valid decoded frame */
int ltc_get_frame(ltc_decoder_t *decoder, ltc_frame_t *frame);

/* Cleanup decoder resources */
void ltc_decoder_cleanup(ltc_decoder_t *decoder);

/* Convert LTC frame to human-readable string "HH:MM:SS:FF" */
void ltc_frame_to_string(const ltc_frame_t *frame, char *buf, size_t buflen);

/* Convert LTC frame + system time to HH:MM:SS:FF with timezone support */
void ltc_frame_to_string_with_tz(const ltc_frame_t *frame, const struct tm *tm_local,
                                 char *buf, size_t buflen);

#endif /* LTC_TIMECODE_H */

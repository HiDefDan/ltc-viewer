#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ltc.h>              /* System libltc header */
#include "ltc-timecode.h"     /* Our project LTC header */

/* LTC bi-phase bit decoding via libltc:
   An LTC frame is 80 bits transmitted at twice the audio rate.
   Bit period = 1 / (2 * frame_rate * 25) seconds
   For 25 fps: 1 bit = 1.6 ms
   For 50 fps: 1 bit = 0.8 ms
   For 60 fps: 1 bit = 0.667 ms
*/

int ltc_decoder_init(ltc_decoder_t *decoder, uint32_t frame_rate) {
    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_rate = frame_rate;
    decoder->valid = 0;
    
    /* Create libltc decoder: 30 audio-per-video frames, queue 8 decoded frames */
    decoder->libltc = (void *)ltc_decoder_create(30, 8);
    if (!decoder->libltc) {
        fprintf(stderr, "[LTC] Failed to create libltc decoder\n");
        return -1;
    }
    
    printf("[LTC] Decoder initialized for %u fps\n", frame_rate);
    return 0;
}

/* Feed ALSA audio samples (mono or multi-channel) to decoder.
   samples: interleaved audio buffer
   count: number of samples per channel
   channels: 1 (mono) or 2 (stereo) */
int ltc_feed_audio(ltc_decoder_t *decoder, const int16_t *samples, 
                   uint32_t count, int channels) {
    if (!decoder || !decoder->libltc) {
        return -1;
    }
    
    LTCDecoder *d = (LTCDecoder *)decoder->libltc;
    
    /* Convert samples to ltc format (unsigned 8-bit) and feed to decoder */
    for (uint32_t i = 0; i < count; i++) {
        /* Extract mono or left channel */
        int16_t sample = samples[i * channels];
        /* Convert from signed int16 to unsigned uint8 (0-255 range) */
        /* Shift: -32768..32767 -> 0..255 */
        unsigned char byte = (unsigned char)(((int32_t)sample + 32768) >> 8);
        
        /* Feed byte to libltc decoder */
        ltc_decoder_write(d, &byte, 1, decoder->frame_rate);
    }
    
    return 0;
}

int ltc_get_frame(ltc_decoder_t *decoder, ltc_frame_t *frame) {
    if (!decoder || !decoder->libltc || !frame) {
        return -1;
    }
    
    LTCDecoder *d = (LTCDecoder *)decoder->libltc;
    LTCFrameExt frame_ext;
    
    /* Try to read a decoded frame from the libltc queue */
    if (ltc_decoder_read(d, &frame_ext) == 0) {
        /* Got a valid frame. Convert LTCFrame to SMPTETimecode. */
        SMPTETimecode stime;
        ltc_frame_to_time(&stime, &frame_ext.ltc, 0);
        
        frame->hours = stime.hours;
        frame->minutes = stime.mins;
        frame->seconds = stime.secs;
        frame->frame = stime.frame;
        frame->days = stime.days;
        
        decoder->valid = 1;
        memcpy(&decoder->last_decoded, frame, sizeof(*frame));
        return 0;
    }
    
    /* No frame available */
    return -1;
}

/* Stub: GPIO edge decoder (deprecated, use ltc_feed_audio for ALSA ingest) */
int ltc_feed_edge(ltc_decoder_t *decoder, uint64_t edge_time_us, int level) {
    (void)decoder;
    (void)edge_time_us;
    (void)level;
    /* No-op: ALSA ingest path supersedes GPIO edge capture */
    return 0;
}

/* Cleanup decoder resources */
void ltc_decoder_cleanup(ltc_decoder_t *decoder) {
    if (decoder && decoder->libltc) {
        LTCDecoder *d = (LTCDecoder *)decoder->libltc;
        ltc_decoder_free(d);
        decoder->libltc = NULL;
    }
}

void ltc_frame_to_string(const ltc_frame_t *frame, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%02u.%02u.%02u.%02u",
             frame->hours, frame->minutes, frame->seconds, frame->frame);
}

void ltc_frame_to_string_with_tz(const ltc_frame_t *frame, const struct tm *tm_local,
                                 char *buf, size_t buflen) {
    /* Format system time as HH.MM.SS.cc (centiseconds from wall clock) */
    struct timespec ts_wall;
    clock_gettime(CLOCK_REALTIME, &ts_wall);
    unsigned int cs = (unsigned int)(ts_wall.tv_nsec / 10000000);
    snprintf(buf, buflen, "%02d.%02d.%02d.%02u",
             tm_local->tm_hour, tm_local->tm_min, tm_local->tm_sec, cs);
}

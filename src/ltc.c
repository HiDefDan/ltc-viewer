#include "ltc.h"
#include <stdio.h>
#include <string.h>

/* LTC bi-phase bit decoding:
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
    return 0;
}

int ltc_feed_edge(ltc_decoder_t *decoder, uint64_t edge_time_us, int level) {
    /* Stub: real implementation would:
       1. Measure time since last edge
       2. Detect bi-phase coding (transitions)
       3. Extract bits and accumulate into 80-bit frame
       4. Check parity and sync words
       5. Decode HH:MM:SS:FF
       
       For now, return placeholder.
    */
    (void)decoder;
    (void)edge_time_us;
    (void)level;
    return 0;
}

int ltc_get_frame(ltc_decoder_t *decoder, ltc_frame_t *frame) {
    if (!decoder->valid) {
        return -1;
    }
    memcpy(frame, &decoder->last_decoded, sizeof(*frame));
    return 0;
}

void ltc_frame_to_string(const ltc_frame_t *frame, char *buf, size_t buflen) {
    snprintf(buf, buflen, "%02u.%02u.%02u",
             frame->hours, frame->minutes, frame->seconds);
}

void ltc_frame_to_string_with_tz(const ltc_frame_t *frame, const struct tm *tm_local,
                                 char *buf, size_t buflen) {
    /* Format system time as HH.MM.SS */
    snprintf(buf, buflen, "%02d.%02d.%02d",
             tm_local->tm_hour, tm_local->tm_min, tm_local->tm_sec);
}

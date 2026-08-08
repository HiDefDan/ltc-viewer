#ifndef LTC_AES67_H
#define LTC_AES67_H

#include <stdint.h>
#include <pthread.h>
#include "ltc-timecode.h"

/* Minimal, hand-rolled AES67-style multicast RTP receiver.
 *
 * Deliberately narrow scope for a v1: one fixed multicast group/port, no
 * PTP, no RTCP, no SAP/SDP discovery — the group/port are supplied by
 * whoever starts the thread (env var for now; see main.c LTC_INPUT_SOURCE/
 * LTC_AES67_GROUP/LTC_AES67_PORT). Parses the 12-byte RTP header and feeds
 * audio/L24 (24-bit big-endian PCM per RFC 3190) payload straight into the
 * existing LTC decoder via ltc_feed_and_publish(), the same publish path
 * the RtAudio/ALSA callback uses — see main.c. Genuinely verified only
 * against ffmpeg's rtp muxer as a test source (see the aes67-ltc-over-ip
 * memory note); not yet validated against real Dante/AES67 hardware, and
 * Dante Virtual Soundcard specifically cannot be used as a source at all
 * (no multicast, no AES67 mode, confirmed 2026-08-08).
 *
 * Only ever run instead of the RtAudio/ALSA path, never alongside it —
 * ltc_decoder_t is not safe for concurrent feeding from two threads. */
typedef struct {
    ltc_decoder_t   *decoder;
    ltc_shared_t    *shared;
    pthread_mutex_t *shared_mutex;
    char             group[64];
    unsigned int     port;
    /* Cleared by the caller to ask the thread to exit; checked at least
     * once per receive-timeout interval (see aes67.c), so shutdown is
     * bounded even with no traffic arriving. */
    volatile int     running;
} aes67_ctx_t;

/* Thread entry point (pass an aes67_ctx_t*, cast to void* for pthread_create).
 * Returns once ctx->running is cleared. Logs and returns immediately on
 * socket/multicast-join setup failure — caller should treat that as fatal,
 * same as an RtAudio open-stream failure. */
void *aes67_receiver_thread(void *arg);

#endif /* LTC_AES67_H */

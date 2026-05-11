#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <math.h>
#include <ltc.h>              /* System libltc header */
#include "ltc-timecode.h"     /* Our project LTC header */
#include "config.h"

static int smpte_time_is_sane(const SMPTETimecode *stime) {
    if (!stime) {
        return 0;
    }
    if (stime->hours > 23) {
        return 0;
    }
    if (stime->mins > 59) {
        return 0;
    }
    if (stime->secs > 59) {
        return 0;
    }
    if (stime->frame > 79) {
        return 0;
    }
    return 1;
}

static uint32_t nearest_standard_fps(float fps_hint, uint32_t fallback) {
    const uint32_t std_fps[3] = {24, 25, 30};
    float best_err = 1e9f;
    uint32_t best = (fallback > 0) ? fallback : 25;

    for (int i = 0; i < 3; i++) {
        float err = fabsf(fps_hint - (float)std_fps[i]);
        if (err < best_err) {
            best_err = err;
            best = std_fps[i];
        }
    }

    return best;
}

static int is_next_second(uint32_t ph, uint32_t pm, uint32_t ps,
                          uint32_t nh, uint32_t nm, uint32_t ns) {
    uint32_t th = ph;
    uint32_t tm = pm;
    uint32_t ts = ps + 1;
    if (ts >= 60) {
        ts = 0;
        tm++;
        if (tm >= 60) {
            tm = 0;
            th = (th + 1) % 24;
        }
    }
    return (th == nh && tm == nm && ts == ns);
}

static int is_next_minute(uint32_t ph, uint32_t pm,
                          uint32_t nh, uint32_t nm) {
    uint32_t th = ph;
    uint32_t tm = pm + 1;
    if (tm >= 60) {
        tm = 0;
        th = (th + 1) % 24;
    }
    return (th == nh && tm == nm);
}

static int frame_pair_continuity_ok(const ltc_frame_t *prev,
                                    const ltc_frame_t *next,
                                    uint32_t nominal_fps) {
    if (!prev || !next || nominal_fps == 0) {
        return 0;
    }

    if (prev->hours == next->hours &&
        prev->minutes == next->minutes &&
        prev->seconds == next->seconds) {
        uint32_t delta = (next->frame + nominal_fps - (prev->frame % nominal_fps)) % nominal_fps;
        return (delta >= 1 && delta <= 2);
    }

    if (is_next_second(prev->hours, prev->minutes, prev->seconds,
                       next->hours, next->minutes, next->seconds)) {
        uint32_t pf = prev->frame % nominal_fps;
        return (pf >= nominal_fps - 2 && next->frame <= 2);
    }

    /* 29.97 DF special case: skip frame numbers 00 and 01 at each minute
     * except every 10th minute. Legal transition is 59:29 -> 00:02. */
    if (nominal_fps == 30 && prev->drop_frame && next->drop_frame &&
        prev->seconds == 59 && next->seconds == 0 &&
        is_next_minute(prev->hours, prev->minutes, next->hours, next->minutes)) {
        if ((next->minutes % 10) != 0) {
            return (prev->frame >= 28 && next->frame >= 2 && next->frame <= 4);
        }
        return (prev->frame >= 28 && next->frame <= 2);
    }

    return 0;
}

static uint32_t frame_index_24h(const ltc_frame_t *f, uint32_t nominal_fps) {
    if (!f || nominal_fps == 0) {
        return 0;
    }
    uint32_t sec_of_day = f->hours * 3600u + f->minutes * 60u + f->seconds;
    return sec_of_day * nominal_fps + (f->frame % nominal_fps);
}

static int frame_delta_24h(const ltc_frame_t *prev,
                           const ltc_frame_t *next,
                           uint32_t nominal_fps) {
    if (!prev || !next || nominal_fps == 0) {
        return 0;
    }

    uint32_t day_frames = 24u * 3600u * nominal_fps;
    uint32_t pi = frame_index_24h(prev, nominal_fps);
    uint32_t ni = frame_index_24h(next, nominal_fps);
    uint32_t forward = (ni + day_frames - pi) % day_frames;
    uint32_t backward = (pi + day_frames - ni) % day_frames;

    if (forward <= backward) {
        return (int)forward;
    }
    return -(int)backward;
}

/* LTC bi-phase bit decoding via libltc:
   An LTC frame is 80 bits transmitted at twice the audio rate.
   Bit period = 1 / (2 * frame_rate * 25) seconds
   For 25 fps: 1 bit = 1.6 ms
   For 50 fps: 1 bit = 0.8 ms
   For 60 fps: 1 bit = 0.667 ms
*/

static int ltc_decoder_recreate(ltc_decoder_t *decoder, uint32_t apv) {
    if (!decoder) {
        return -1;
    }
    if (apv < 1) {
        apv = 1;
    }

    if (decoder->libltc) {
        ltc_decoder_free((LTCDecoder *)decoder->libltc);
        decoder->libltc = NULL;
    }

    decoder->libltc = (void *)ltc_decoder_create((int)apv, 32);
    if (!decoder->libltc) {
        return -1;
    }

    decoder->apv_current = apv;
    decoder->apv_mismatch_count = 0;
    return 0;
}

int ltc_decoder_init(ltc_decoder_t *decoder, uint32_t frame_rate) {
    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_rate = frame_rate;
    /* Keep validation fps tied to configured source to avoid runtime mis-snaps
     * (e.g. 30fps source being treated as 25fps from noisy span estimates). */
    decoder->nominal_fps = (frame_rate > 0) ? frame_rate : 25;
    decoder->valid = 0;
    
    /* Create libltc decoder using audio frames per LTC frame (APV). */
    int apv = (frame_rate > 0) ? (int)(ALSA_CAPTURE_RATE / frame_rate) : 0;
    if (apv < 1) {
        apv = 1;
    }
    if (ltc_decoder_recreate(decoder, (uint32_t)apv) != 0) {
        fprintf(stderr, "[LTC] Failed to create libltc decoder\n");
        return -1;
    }
    
    printf("[LTC] Decoder initialized for %u fps\n", frame_rate);
    return 0;
}

/* Feed ALSA audio samples (mono U8, already converted from S32 capture) to decoder.
   samples: mono U8 buffer
   count: number of samples
   channels: ignored (mono assumed after mixing in caller) */
int ltc_feed_audio(ltc_decoder_t *decoder, const int16_t *samples,
                   uint32_t count, int channels) {
    (void)channels;
    if (!decoder || !decoder->libltc || !samples || count == 0) {
        return -1;
    }
    LTCDecoder *d = (LTCDecoder *)decoder->libltc;
    ltc_decoder_write_s16(d, (int16_t *)samples, count, (ltc_off_t)decoder->sample_pos);
    decoder->sample_pos += count;

    /* If input vanished for a while, force-unlock so restarted input can
     * reacquire immediately without carrying stale continuity state. */
    if (decoder->decoded_count > 0 && !decoder->gap_reset_armed) {
        uint64_t silent_samples = decoder->sample_pos - decoder->last_decode_sample;
        if (silent_samples >= (uint64_t)(ALSA_CAPTURE_RATE * 2)) {
            decoder->valid = 0;
            decoder->lock_streak = 0;
            memset(&decoder->pending_decoded, 0, sizeof(decoder->pending_decoded));
            decoder->gap_reset_armed = 1;
            printf("[LTC] Input gap %.3fs -> reset lock state\n",
                   (double)silent_samples / (double)ALSA_CAPTURE_RATE);
        }
    }
    return 0;
}

int ltc_get_frame(ltc_decoder_t *decoder, ltc_frame_t *frame) {
    /* 2-frame streak: proven to lock reliably on the HiFiBerry loopback path.
       The previous value of 4 was broken by the APV retune storm (see below). */
    const uint32_t lock_threshold = 2;
    const int max_resync_gap = 4;

    if (!decoder || !decoder->libltc || !frame) {
        return -1;
    }
    
    LTCDecoder *d = (LTCDecoder *)decoder->libltc;
    LTCFrameExt frame_ext;
    
    /* Try to read a decoded frame from the libltc queue */
    if (ltc_decoder_read(d, &frame_ext) == 1) {
        decoder->decoded_count++;
        decoder->last_decode_sample = (uint64_t)frame_ext.off_end;
        decoder->gap_reset_armed = 0;

        ltc_off_t span = frame_ext.off_end - frame_ext.off_start + 1;

        /* Decode SMPTE timecode early so APV retune can be gated on sane frames. */
        SMPTETimecode stime;
        ltc_frame_to_time(&stime, &frame_ext.ltc, 0);
        int frame_sane = smpte_time_is_sane(&stime);

        if (span > 0) {
            float inst_fps = (float)ALSA_CAPTURE_RATE / (float)span;
            if (inst_fps > 10.0f && inst_fps < 120.0f) {
                decoder->detected_fps_inst = inst_fps;
                if (decoder->detected_fps <= 0.0f) {
                    decoder->detected_fps = inst_fps;
                } else {
                    decoder->detected_fps = decoder->detected_fps * 0.90f + inst_fps * 0.10f;
                }

                /* Adapt nominal fps to actual incoming LTC cadence and frame numbering.
                 * This allows intentional 23.98/24/25/29.97/30 sources to be
                 * detected from the wire instead of staying pinned to build-time fps. */
                uint32_t prev_nominal = decoder->nominal_fps;
                uint32_t auto_nominal = nearest_standard_fps(decoder->detected_fps,
                                                             decoder->frame_rate);
                uint32_t inferred_nominal = auto_nominal;

                /* Frame index constraints tighten inference:
                 * frame 25..29 can only occur at 30fps.
                 * frame 24 cannot occur at 24fps. */
                if (stime.frame >= 25) {
                    inferred_nominal = 30;
                } else if (stime.frame == 24 && inferred_nominal == 24) {
                    inferred_nominal = 25;
                }

                decoder->nominal_fps = inferred_nominal;

                if (decoder->nominal_fps != prev_nominal) {
                    printf("[LTC] Nominal fps adjusted %u->%u (measured %.2f, frame=%u)\n",
                           prev_nominal, decoder->nominal_fps,
                           decoder->detected_fps, (unsigned)stime.frame);
                    decoder->valid = 0;
                    decoder->lock_streak = 0;
                    memset(&decoder->pending_decoded, 0, sizeof(decoder->pending_decoded));
                }
            }

            /* APV retune: compare raw measured span against current APV.
             * libltc uses APV to calibrate its internal bit-clock PLL. The APV
             * MUST match the actual observed frame period (in samples), not a
             * standardised fps value. Using std-fps APV (e.g. 1600 for 30fps)
             * when the loopback presents frames at ~1349 samples causes a 15%
             * PLL mismatch → libltc rarely finds frame boundaries → almost no
             * decoded frames → no lock possible.
             * After retune we reset lock state so stale pending_decoded cannot
             * cause a false pair-continuity failure on the first post-retune frame. */
            uint32_t apv_est = (uint32_t)llround((double)span);
            uint32_t apv_ref = (decoder->apv_current > 0) ? decoder->apv_current : 1;
            uint32_t apv_diff = (apv_est > apv_ref) ? (apv_est - apv_ref) : (apv_ref - apv_est);

            if (apv_diff > (apv_ref / 8)) { /* >12.5% mismatch */
                if (frame_sane) {
                    /* Only count mismatches from sane frames — garbage frames
                     * have wildly wrong span values that corrupt the APV. */
                    decoder->apv_mismatch_count++;
                    if (decoder->apv_mismatch_count >= 4) {
                        if (ltc_decoder_recreate(decoder, apv_est) == 0) {
                            printf("[LTC] Retuned decoder APV %u->%u (measured %.2f fps)\n",
                                   apv_ref, apv_est, decoder->detected_fps);
                            decoder->valid = 0;
                            decoder->lock_streak = 0;
                            memset(&decoder->pending_decoded, 0, sizeof(decoder->pending_decoded));
                        }
                    }
                }
            } else {
                decoder->apv_mismatch_count = 0;
            }
        }

        if (!frame_sane) {
             printf("[LTC-FRAME] #%" PRIu64 " REJ:sane tc=%d.%02d.%02d.%02d span=%ld fps=%.2f\n",
                 decoder->decoded_count,
                 stime.hours, stime.mins, stime.secs, stime.frame,
                 (long)span, decoder->detected_fps);
            printf("[LTC-DBG] REJ:sane %d.%02d.%02d.%02d\n",
                   stime.hours, stime.mins, stime.secs, stime.frame);
            return -1;
        }

        if (decoder->nominal_fps > 0 && stime.frame >= decoder->nominal_fps) {
             printf("[LTC-FRAME] #%" PRIu64 " REJ:frame tc=%02d.%02d.%02d.%02d nfps=%u\n",
                 decoder->decoded_count,
                 stime.hours, stime.mins, stime.secs, stime.frame,
                 decoder->nominal_fps);
            printf("[LTC-DBG] REJ:frame# %d>=nfps%u\n",
                   stime.frame, decoder->nominal_fps);
            return -1;
        }

        ltc_frame_t candidate = {
            .hours = stime.hours,
            .minutes = stime.mins,
            .seconds = stime.secs,
            .frame = stime.frame,
            .days = stime.days,
            .drop_frame = frame_ext.ltc.dfbit ? 1 : 0,
            .sample_off_start = (uint64_t)frame_ext.off_start,
            .sample_off_end = (uint64_t)frame_ext.off_end,
        };

        const ltc_frame_t *seq_ref = NULL;
        const char *seq_src = "none";
        if (decoder->valid) {
            seq_ref = &decoder->last_decoded;
            seq_src = "last";
        } else if (decoder->lock_streak > 0) {
            seq_ref = &decoder->pending_decoded;
            seq_src = "pending";
        }
        int seq_delta = seq_ref ? frame_delta_24h(seq_ref, &candidate, decoder->nominal_fps) : 0;
        const char *seq_state = "NA";
        if (seq_ref) {
            if (seq_delta == 1) {
                seq_state = "OK";
            } else if (seq_delta > 1) {
                seq_state = "GAP";
                decoder->gap_count++;
            } else if (seq_delta == 0) {
                seq_state = "DUP";
            } else {
                seq_state = "BACK";
            }
        }

        printf("[LTC-FRAME] #%" PRIu64 " seq=%s d=%d src=%s tc=%02u.%02u.%02u.%02u span=%ld fps=%.2f nfps=%u valid=%d streak=%u\n",
               decoder->decoded_count,
               seq_state,
               seq_delta,
               seq_src,
               candidate.hours, candidate.minutes, candidate.seconds, candidate.frame,
               (long)span,
               decoder->detected_fps,
               decoder->nominal_fps,
               decoder->valid,
               decoder->lock_streak);

        if (decoder->valid) {
            if (!frame_pair_continuity_ok(&decoder->last_decoded, &candidate, decoder->nominal_fps)) {
                int delta = frame_delta_24h(&decoder->last_decoded, &candidate, decoder->nominal_fps);

                /* Forward jumps can happen with noisy/attenuated inputs.
                 * Prefer seamless resync over lock-drop churn on stage displays. */
                if (delta > 0 && delta <= max_resync_gap) {
                    printf("[LTC] Gap resync +%d at %02u.%02u.%02u.%02u\n",
                           delta,
                           candidate.hours, candidate.minutes, candidate.seconds, candidate.frame);
                    decoder->last_decoded = candidate;
                    decoder->lock_streak = lock_threshold;
                    *frame = candidate;
                    return 0;
                }

                printf("[LTC] Lock dropped at %02u.%02u.%02u.%02u\n",
                       candidate.hours, candidate.minutes, candidate.seconds, candidate.frame);
                decoder->valid = 0;
                decoder->lock_streak = 1;
                decoder->pending_decoded = candidate;
                return -1;
            }

            decoder->lock_streak = lock_threshold;
            decoder->last_decoded = candidate;
            *frame = candidate;
            return 0;
        }

        if (decoder->lock_streak == 0) {
            decoder->pending_decoded = candidate;
            decoder->lock_streak = 1;
            return -1;
        }

        if (!frame_pair_continuity_ok(&decoder->pending_decoded, &candidate, decoder->nominal_fps)) {
            printf("[LTC-DBG] REJ:pair %02u.%02u.%02u.%02u->%02u.%02u.%02u.%02u nfps=%u streak=%u\n",
                   decoder->pending_decoded.hours, decoder->pending_decoded.minutes,
                   decoder->pending_decoded.seconds, decoder->pending_decoded.frame,
                   candidate.hours, candidate.minutes, candidate.seconds, candidate.frame,
                   decoder->nominal_fps, decoder->lock_streak);
            decoder->pending_decoded = candidate;
            decoder->lock_streak = 1;
            return -1;
        }

        decoder->pending_decoded = candidate;
        decoder->lock_streak++;
        if (decoder->lock_streak < lock_threshold) {
            return -1;
        }

        decoder->valid = 1;
        decoder->last_decoded = candidate;
        *frame = candidate;
        printf("[LTC] Lock acquired at %02u.%02u.%02u.%02u (fps %.2f)\n",
               candidate.hours, candidate.minutes, candidate.seconds, candidate.frame,
               decoder->detected_fps);
        return 0;
    }
    
    /* No frame available */
    return -1;
}

float ltc_get_detected_fps(const ltc_decoder_t *decoder) {
    if (!decoder) {
        return 0.0f;
    }
    return decoder->detected_fps;
}

uint32_t ltc_get_nominal_fps(const ltc_decoder_t *decoder) {
    if (!decoder) {
        return 0;
    }
    return decoder->nominal_fps;
}

uint64_t ltc_get_gap_count(const ltc_decoder_t *decoder) {
    if (!decoder) {
        return 0;
    }
    return decoder->gap_count;
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
    (void)frame;
    /* Format system time as HH.MM.SS.cc (centiseconds from wall clock) */
    struct timespec ts_wall;
    clock_gettime(CLOCK_REALTIME, &ts_wall);
    unsigned int cs = (unsigned int)(ts_wall.tv_nsec / 10000000);
    snprintf(buf, buflen, "%02d.%02d.%02d.%02u",
             tm_local->tm_hour, tm_local->tm_min, tm_local->tm_sec, cs);
}

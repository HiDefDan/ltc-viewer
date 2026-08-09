#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "aes67.h"

/* Sized for a full Ethernet-MTU UDP payload (~1500 bytes) with headroom;
 * observed ffmpeg packets during testing were 1470 bytes total. */
#define AES67_RECV_BUF_SIZE 2048
#define AES67_SAMPLE_BUF_SIZE (AES67_RECV_BUF_SIZE / 3 + 1)

/* How often the receive loop wakes up with no packet, purely to re-check
 * ctx->running — bounds shutdown latency even with a dead/absent sender. */
#define AES67_RECV_TIMEOUT_MS 500

void *aes67_receiver_thread(void *arg)
{
    aes67_ctx_t *ctx = (aes67_ctx_t *)arg;
    if (!ctx) {
        return NULL;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        fprintf(stderr, "[AES67] socket() failed: %s\n", strerror(errno));
        return NULL;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((uint16_t)ctx->port);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "[AES67] bind() to port %u failed: %s\n", ctx->port, strerror(errno));
        close(sock);
        return NULL;
    }

    /* Multicast vs unicast is decided from the address itself: a multicast
     * address (224.0.0.0/4) needs an explicit group join, a unicast one
     * needs nothing — the INADDR_ANY bind above already receives packets
     * addressed to this host. Supporting both matters in practice: a cheap
     * unmanaged switch can enforce a multicast forwarding ceiling far below
     * line rate (one here caps at ~300pps and mangled a 1000pps stream),
     * where the identical stream sent unicast arrives complete. */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, ctx->group, &mreq.imr_multiaddr) != 1) {
        fprintf(stderr, "[AES67] invalid listen address '%s'\n", ctx->group);
        close(sock);
        return NULL;
    }
    int joined_multicast = 0;
    if (IN_MULTICAST(ntohl(mreq.imr_multiaddr.s_addr))) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            fprintf(stderr, "[AES67] failed to join multicast group %s: %s\n",
                    ctx->group, strerror(errno));
            close(sock);
            return NULL;
        }
        joined_multicast = 1;
    }

    struct timeval rcv_timeout;
    rcv_timeout.tv_sec = AES67_RECV_TIMEOUT_MS / 1000;
    rcv_timeout.tv_usec = (AES67_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

    printf("[AES67] Listening on %s:%u (%s)\n", ctx->group, ctx->port,
           joined_multicast ? "multicast" : "unicast");
    fflush(stdout);

    uint8_t recv_buf[AES67_RECV_BUF_SIZE];
    int16_t sample_buf[AES67_SAMPLE_BUF_SIZE];
    int have_last_seq = 0;
    uint16_t last_seq = 0;
    int logged_payload_type = -1;
    uint64_t packets_received = 0;
    uint64_t seq_gaps = 0;

    while (ctx->running) {
        ssize_t n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
        if (n < 0) {
            /* EAGAIN/EWOULDBLOCK from SO_RCVTIMEO is the normal "nothing
             * arrived, go re-check ctx->running" path, not an error. */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "[AES67] recvfrom() error: %s\n", strerror(errno));
            }
            continue;
        }

        struct timespec ts_ingest;
        clock_gettime(CLOCK_MONOTONIC, &ts_ingest);
        uint64_t ingest_ns = (uint64_t)ts_ingest.tv_sec * 1000000000ULL
                            + (uint64_t)ts_ingest.tv_nsec;

        if (n < 12) {
            fprintf(stderr, "[AES67] packet too short to be RTP (%zd bytes)\n", n);
            continue;
        }

        const uint8_t *p = recv_buf;
        int version = (p[0] >> 6) & 0x3;
        int has_extension = (p[0] >> 4) & 0x1;
        int cc = p[0] & 0xF;
        int payload_type = p[1] & 0x7F;
        uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);

        if (version != 2) {
            fprintf(stderr, "[AES67] unexpected RTP version %d, dropping packet\n", version);
            continue;
        }

        size_t header_len = 12 + (size_t)cc * 4;
        if ((size_t)n < header_len) {
            fprintf(stderr, "[AES67] packet shorter than its own CSRC-extended header, dropping\n");
            continue;
        }

        if (has_extension) {
            if ((size_t)n < header_len + 4) {
                fprintf(stderr, "[AES67] truncated RTP extension header, dropping\n");
                continue;
            }
            uint16_t ext_words = (uint16_t)((recv_buf[header_len + 2] << 8) | recv_buf[header_len + 3]);
            header_len += 4 + (size_t)ext_words * 4;
            if ((size_t)n < header_len) {
                fprintf(stderr, "[AES67] truncated RTP extension payload, dropping\n");
                continue;
            }
        }

        if (payload_type != logged_payload_type) {
            printf("[AES67] payload type now %d (was %d) — not validated against a config value, "
                   "informational only\n", payload_type, logged_payload_type);
            logged_payload_type = payload_type;
        }

        if (have_last_seq) {
            uint16_t expected = (uint16_t)(last_seq + 1);
            if (seq != expected) {
                uint16_t lost = (uint16_t)(seq - expected);
                seq_gaps++;
                fprintf(stderr, "[AES67] sequence gap: expected %u got %u (%u packet(s) lost, "
                        "%" PRIu64 " gaps so far)\n", expected, seq, lost, seq_gaps);
            }
        }
        have_last_seq = 1;
        last_seq = seq;
        packets_received++;

        const uint8_t *payload = recv_buf + header_len;
        size_t payload_len = (size_t)n - header_len;
        if (payload_len % 3 != 0) {
            fprintf(stderr, "[AES67] payload length %zu not a multiple of 3 (expected L24) — "
                    "truncating trailing partial sample\n", payload_len);
        }
        size_t num_samples = payload_len / 3;
        if (num_samples > AES67_SAMPLE_BUF_SIZE) {
            fprintf(stderr, "[AES67] packet carries more samples (%zu) than the conversion buffer "
                    "holds (%d) — truncating; a real jumbo/oversized packet or a bug\n",
                    num_samples, AES67_SAMPLE_BUF_SIZE);
            num_samples = AES67_SAMPLE_BUF_SIZE;
        }

        /* L24 is 24-bit big-endian PCM (RFC 3190 audio/L24). Truncate to
         * the top 16 bits: the MSB carries the sign, so a plain cast of
         * the top two bytes reinterpreted as int16 is already correctly
         * signed — no separate sign-extension step needed. LTC is a
         * bilevel/FM-encoded signal, so losing the bottom 8 bits of
         * resolution here doesn't matter. */
        for (size_t i = 0; i < num_samples; i++) {
            uint8_t b0 = payload[i * 3];
            uint8_t b1 = payload[i * 3 + 1];
            sample_buf[i] = (int16_t)((b0 << 8) | b1);
        }

        ltc_feed_and_publish(ctx->decoder, ctx->shared, ctx->shared_mutex,
                             sample_buf, (unsigned int)num_samples, ingest_ns);
    }

    printf("[AES67] Stopping (received %" PRIu64 " packets, %" PRIu64 " sequence gaps)\n",
           packets_received, seq_gaps);
    fflush(stdout);

    if (joined_multicast) {
        setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    }
    close(sock);
    return NULL;
}

/* Behavioral tests for udp_stream_send_nal(): RTP marker bit and timestamp
   handling over a real loopback UDP socket.

   Two invariants per RFC 6184 (H.264) / RFC 7798 (H.265):
     1. The marker bit flags the LAST packet of an access unit only — set on
        the last NAL of a frame (and, when fragmented, only on its last FU).
     2. Every packet of one frame shares a single RTP timestamp, and the
        timestamp advances between frames. */

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "stream.h"

#define RTP_HDR 12

/* Loopback UDP socket bound to an ephemeral port, with a receive timeout so a
   missing packet fails the test instead of hanging it. Returns the port. */
static unsigned short recv_sock(int *fd_out) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(fd, (struct sockaddr *)&addr, sizeof(addr)));
    socklen_t alen = sizeof(addr);
    assert(!getsockname(fd, (struct sockaddr *)&addr, &alen));
    struct timeval tv = {.tv_sec = 2};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
    *fd_out = fd;
    return ntohs(addr.sin_port);
}

static int rtp_marker(const unsigned char *p) { return (p[1] & 0x80) ? 1 : 0; }
static unsigned int rtp_ts(const unsigned char *p) {
    return ((unsigned)p[4] << 24) | ((unsigned)p[5] << 16) |
           ((unsigned)p[6] << 8) | p[7];
}

/* Receive one RTP datagram; assert it arrived. */
static int recv_pkt(int fd, unsigned char *buf, int cap) {
    int n = recvfrom(fd, buf, cap, 0, NULL, NULL);
    assert(n >= RTP_HDR && "expected an RTP packet within the timeout");
    return n;
}

/* Send one NAL of nal_size that must fragment, then validate the FU stream:
   correct per-codec FU header, marker only on the final fragment, and a
   payload that reassembles to exactly the original NAL body. Exercises the
   off-by-one fragment count (a too-small nal_size used to drive payload_size
   negative -> memcpy with a huge size) and the RFC 7798 3-byte H.265 FU. */
static void verify_fu(int rfd, int is_h265, int nal_size) {
    unsigned char nal[8192];
    assert(nal_size <= (int)sizeof(nal));
    for (int i = 0; i < nal_size; i++) nal[i] = (unsigned char)(i & 0xFF);
    nal[0] = is_h265 ? 0x26 : 0x65;   /* H.265 IDR_W_RADL / H.264 IDR slice */
    nal[1] = 0x01;                    /* H.265 layer 0, TID 1 */
    int hdr = is_h265 ? 2 : 1;        /* original NAL header skipped by FU */
    int fu = is_h265 ? 3 : 2;         /* FU header bytes prepended per fragment */
    unsigned char want_type = is_h265 ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);

    udp_stream_send_nal((char *)nal, nal_size, 1, is_h265);

    unsigned char pkt[2048], body[8192];
    int body_len = 0, frags = 0, markers = 0;
    for (;;) {
        int n = recvfrom(rfd, pkt, sizeof(pkt), 0, NULL, NULL);
        if (n < RTP_HDR) break;                 /* timeout: no more fragments */
        int first = (frags == 0);
        markers += rtp_marker(pkt);
        if (is_h265) {
            assert(((pkt[RTP_HDR] >> 1) & 0x3F) == 49);     /* FU NAL type */
            assert(pkt[RTP_HDR + 1] == nal[1]);             /* layer/TID preserved */
            assert((pkt[RTP_HDR + 2] & 0x3F) == want_type); /* original type */
            assert(!!(pkt[RTP_HDR + 2] & 0x80) == first);   /* S only on first */
        } else {
            assert((pkt[RTP_HDR] & 0x1F) == 28);            /* FU-A type */
            assert((pkt[RTP_HDR + 1] & 0x1F) == want_type);
            assert(!!(pkt[RTP_HDR + 1] & 0x80) == first);
        }
        int plen = n - RTP_HDR - fu;
        assert(plen >= 0 && body_len + plen <= (int)sizeof(body));
        memcpy(body + body_len, pkt + RTP_HDR + fu, plen);
        body_len += plen;
        frags++;
    }
    assert(frags >= 2 && "nal_size must force fragmentation");
    assert(markers == 1 && "exactly one fragment carries the marker");
    assert(rtp_marker(pkt) == 1 && "the marker is on the final fragment");
    assert(body_len == nal_size - hdr && "fragments reassemble to the NAL body");
    assert(!memcmp(body, nal + hdr, body_len) && "reassembled payload matches");
    printf("  FU %s nal=%d -> %d frags, reassembled, marker on last: OK\n",
           is_h265 ? "H265" : "H264", nal_size, frags);
}

int main(void) {
    int rfd;
    unsigned short port = recv_sock(&rfd);

    assert(udp_stream_init(0, NULL) == EXIT_SUCCESS);
    assert(udp_stream_add_client("127.0.0.1", port) >= 0);

    unsigned char pkt[4096];
    char nal[100];
    memset(nal, 0x41, sizeof(nal));  /* H.264 non-IDR slice header byte */

    /* --- Frame of three single-packet NALs: marker only on the last --- */
    udp_stream_send_nal(nal, sizeof(nal), 0, 0);
    udp_stream_send_nal(nal, sizeof(nal), 0, 0);
    udp_stream_send_nal(nal, sizeof(nal), 1, 0);

    recv_pkt(rfd, pkt, sizeof(pkt));
    unsigned int f1_ts = rtp_ts(pkt);
    assert(rtp_marker(pkt) == 0);
    recv_pkt(rfd, pkt, sizeof(pkt));
    assert(rtp_marker(pkt) == 0);
    assert(rtp_ts(pkt) == f1_ts);
    recv_pkt(rfd, pkt, sizeof(pkt));
    assert(rtp_marker(pkt) == 1);            /* last NAL of the AU */
    assert(rtp_ts(pkt) == f1_ts);            /* same timestamp across the frame */
    printf("  marker set only on last NAL; one timestamp per frame: OK\n");

    /* --- Next frame: timestamp must advance, again shared across NALs --- */
    udp_stream_send_nal(nal, sizeof(nal), 0, 0);
    udp_stream_send_nal(nal, sizeof(nal), 1, 0);

    recv_pkt(rfd, pkt, sizeof(pkt));
    unsigned int f2_ts = rtp_ts(pkt);
    assert(rtp_marker(pkt) == 0);
    recv_pkt(rfd, pkt, sizeof(pkt));
    assert(rtp_marker(pkt) == 1);
    assert(rtp_ts(pkt) == f2_ts);
    assert((int)(f2_ts - f1_ts) > 0 && "timestamp must advance between frames");
    printf("  timestamp advances between frames: OK\n");

    /* --- Fragmentation: H.264 FU-A and H.265 FU, incl. the off-by-one size
       (nal_size just over one chunk) that used to drive payload_size negative --- */
    verify_fu(rfd, 0, 3500);   /* H.264, several fragments */
    verify_fu(rfd, 1, 3500);   /* H.265, RFC 7798 3-byte FU header */
    verify_fu(rfd, 0, 2796);   /* H.264: body 2795 = 2*1398 - 1, old code went negative */
    verify_fu(rfd, 1, 2795);   /* H.265: body 2793 = 2*1397 - 1, old code went negative */

    udp_stream_close();
    close(rfd);
    printf("test_stream_rtp: OK\n");
    return 0;
}

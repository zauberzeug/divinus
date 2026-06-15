/* Unit tests for the shared packetizer core (src/fmt/rtppkt.c) that both the
   RTSP and UDP senders build on. Verifies the single-NAL vs fragmentation
   boundary, RFC 6184 (H.264 FU-A) / RFC 7798 (H.265 FU) header bytes, the
   marker riding only the final packet, exact reassembly, and that no fragment
   size ever goes negative. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "fmt/rtppkt.h"

struct pkt {
    int hdr_len;
    unsigned char hdr[3];
    int body_len;
    const unsigned char *body;
    int marker;
};

struct cap {
    struct pkt p[512];
    int n;
};

static int cap_emit(void *ctx, const unsigned char *hdr, int hdr_len,
                    const unsigned char *body, int body_len, int marker) {
    struct cap *c = ctx;
    assert(c->n < (int)(sizeof(c->p) / sizeof(c->p[0])));
    struct pkt *q = &c->p[c->n++];
    q->hdr_len = hdr_len;
    if (hdr_len) memcpy(q->hdr, hdr, hdr_len);
    q->body = body;
    q->body_len = body_len;
    q->marker = marker;
    return 0;
}

/* A NAL that fits within max_payload is emitted as a single packet: no FU
   header, body is the whole NAL, marker carries the access-unit flag. */
static void test_single(void) {
    unsigned char nal[100];
    for (int i = 0; i < (int)sizeof(nal); i++) nal[i] = i;
    nal[0] = 0x26; nal[1] = 0x01;   /* H.265 IDR */

    struct cap c = {0};
    assert(rtp_packetize_nal(nal, sizeof(nal), 1, 1388, 1, cap_emit, &c) == 0);
    assert(c.n == 1);
    assert(c.p[0].hdr_len == 0);
    assert(c.p[0].body_len == (int)sizeof(nal));
    assert(c.p[0].body == nal);
    assert(c.p[0].marker == 1);

    struct cap c0 = {0};
    rtp_packetize_nal(nal, sizeof(nal), 1, 1388, 0, cap_emit, &c0);
    assert(c0.n == 1 && c0.p[0].marker == 0 && "non-AU-final NAL is unmarked");
    printf("  single packet: no FU header, marker reflects AU end: OK\n");
}

/* Fragment one NAL and check: per-codec FU headers, S only on the first packet,
   E + marker only on the last, body reassembles to the original NAL payload. */
static void verify_fu(int is_h265) {
    int head = is_h265 ? 3 : 2;
    int nal_hdr = is_h265 ? 2 : 1;
    int max_payload = 20;                 /* tiny, to force several fragments */
    unsigned char nal[50];
    for (int i = 0; i < (int)sizeof(nal); i++) nal[i] = (unsigned char)(i + 1);
    nal[0] = is_h265 ? 0x26 : 0x65;       /* IDR */
    nal[1] = 0x01;
    unsigned int type = is_h265 ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);

    struct cap c = {0};
    assert(rtp_packetize_nal(nal, sizeof(nal), is_h265, max_payload, 1,
                             cap_emit, &c) == 0);
    assert(c.n >= 2 && "must fragment");

    unsigned char body[64];
    int body_len = 0, markers = 0;
    for (int i = 0; i < c.n; i++) {
        struct pkt *q = &c.p[i];
        int first = (i == 0), last = (i == c.n - 1);
        assert(q->hdr_len == head);
        assert(q->hdr_len + q->body_len <= max_payload && "payload within MTU");
        if (is_h265) {
            assert(((q->hdr[0] >> 1) & 0x3F) == 49);     /* FU type */
            assert(q->hdr[0] == ((49 << 1) | (nal[0] & 0x81)));
            assert(q->hdr[1] == nal[1]);                 /* layer/TID preserved */
            assert((q->hdr[2] & 0x3F) == type);
            assert(!!(q->hdr[2] & 0x80) == first);       /* S */
            assert(!!(q->hdr[2] & 0x40) == last);        /* E */
        } else {
            assert((q->hdr[0] & 0x1F) == 28);            /* FU-A type */
            assert(q->hdr[0] == (28 | (nal[0] & 0xE0)));
            assert((q->hdr[1] & 0x1F) == type);
            assert(!!(q->hdr[1] & 0x80) == first);
            assert(!!(q->hdr[1] & 0x40) == last);
        }
        markers += q->marker;
        assert(q->marker == last && "marker only on the final fragment");
        memcpy(body + body_len, q->body, q->body_len);
        body_len += q->body_len;
    }
    assert(markers == 1);
    assert(body_len == (int)sizeof(nal) - nal_hdr);
    assert(!memcmp(body, nal + nal_hdr, body_len) && "fragments reassemble exactly");
    printf("  FU %s: headers, S/E, marker on last, exact reassembly: OK\n",
           is_h265 ? "H265" : "H264");
}

/* The single<->fragment decision is exactly at max_payload, and a NAL one byte
   over the boundary fragments without driving any fragment size negative. */
static void test_boundary(void) {
    unsigned char nal[2048];
    for (int i = 0; i < (int)sizeof(nal); i++) nal[i] = (unsigned char)i;
    nal[0] = 0x65;   /* H.264 IDR */

    struct cap c = {0};
    rtp_packetize_nal(nal, 1388, 0, 1388, 1, cap_emit, &c);
    assert(c.n == 1 && c.p[0].hdr_len == 0 && "== max_payload stays single");

    for (int extra = 1; extra <= 3; extra++) {
        struct cap f = {0};
        rtp_packetize_nal(nal, 1388 + extra, 0, 1388, 1, cap_emit, &f);
        assert(f.n >= 2 && "one byte over fragments");
        int total = 0, markers = 0;
        for (int i = 0; i < f.n; i++) {
            assert(f.p[i].body_len > 0 && "fragment size never negative/zero");
            total += f.p[i].body_len;
            markers += f.p[i].marker;
        }
        assert(total == 1388 + extra - 1 && "FU body == NAL minus its 1-byte header");
        assert(markers == 1);
    }
    printf("  boundary at max_payload, no negative fragment sizes: OK\n");
}

/* A NAL too short to hold a header is skipped, not packetized. */
static void test_runt(void) {
    unsigned char nal[3] = {0x26, 0x01, 0x00};
    struct cap c = {0};
    assert(rtp_packetize_nal(nal, sizeof(nal), 1, 1388, 1, cap_emit, &c) == 0);
    assert(c.n == 0 && "runt NAL emits nothing");
    printf("  runt NAL (< 4 bytes) skipped: OK\n");
}

int main(void) {
    test_single();
    verify_fu(0);
    verify_fu(1);
    test_boundary();
    test_runt();
    printf("test_rtppkt: OK\n");
    return 0;
}

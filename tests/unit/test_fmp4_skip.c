/* Contract tests for per-client fMP4 skip timeline behavior.
   Uses a real socketpair and verifies that state advancement survives skipped
   sends so timeline gaps are clean and monotone. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "fmp4_fanout.h"
#include "fmt/mp4.h"

#define FRAG_PAYLOAD_SIZE 1500U
#define TARGET_SKIPS 3

struct MoofFields {
    uint32_t sequence_number;
    uint64_t base_media_decode_time;
};

static uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const unsigned char *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    assert(!fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

static void make_socketpair(int *wfd, int *rfd, int sndbuf) {
    int sv[2] = {-1, -1};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    if (sndbuf > 0)
        assert(!setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);
    *wfd = sv[0];
    *rfd = sv[1];
}

static size_t drain_fd(int fd, unsigned char *out, size_t cap) {
    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, out + total, cap - total, 0);
        if (n > 0) {
            total += (size_t)n;
            assert(total < cap);
            continue;
        }
        if (n == 0)
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        assert(!"unexpected recv error");
    }
    return total;
}

static size_t parse_chunk_len(const unsigned char *buf, size_t len, size_t *hdr_len) {
    size_t pos = 0;
    size_t value = 0;
    assert(len >= 3);
    while (pos < len && buf[pos] != '\r') {
        unsigned char c = buf[pos];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value += (size_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value += (size_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value += (size_t)(c - 'A' + 10);
        } else {
            assert(!"invalid hex digit");
        }
        pos++;
    }
    assert(pos + 1 < len);
    assert(buf[pos] == '\r');
    assert(buf[pos + 1] == '\n');
    *hdr_len = pos + 2;
    return value;
}

static const unsigned char *find_box(const unsigned char *buf, size_t len,
    const char type[4], size_t *box_len) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        size_t size = be32(buf + pos);
        const unsigned char *name = buf + pos + 4;
        assert(size >= 8);
        assert(pos + size <= len);
        if (!memcmp(name, type, 4)) {
            *box_len = size;
            return buf + pos;
        }
        pos += size;
    }
    return NULL;
}

static struct MoofFields parse_moof_fields_from_wire(const unsigned char *wire,
    size_t wire_len) {
    size_t hdr_len = 0;
    size_t moof_len = parse_chunk_len(wire, wire_len, &hdr_len);
    assert(hdr_len + moof_len + 2 <= wire_len);

    const unsigned char *moof = wire + hdr_len;
    assert(!memcmp(moof + 4, "moof", 4));
    assert(be32(moof) == moof_len);

    size_t mfhd_len = 0;
    const unsigned char *mfhd = find_box(moof + 8, moof_len - 8, "mfhd", &mfhd_len);
    assert(mfhd && mfhd_len >= 16);

    size_t traf_len = 0;
    const unsigned char *traf = find_box(moof + 8, moof_len - 8, "traf", &traf_len);
    assert(traf && traf_len >= 8);

    size_t tfdt_len = 0;
    const unsigned char *tfdt = find_box(traf + 8, traf_len - 8, "tfdt", &tfdt_len);
    assert(tfdt && tfdt_len >= 16);

    unsigned char version = tfdt[8];
    uint64_t decode_time = 0;
    if (version == 1) {
        assert(tfdt_len >= 20);
        decode_time = be64(tfdt + 12);
    } else {
        decode_time = be32(tfdt + 12);
    }

    struct MoofFields fields = {
        .sequence_number = be32(mfhd + 12),
        .base_media_decode_time = decode_time,
    };
    return fields;
}

static void stage_fragment(unsigned char tag, char is_iframe) {
    static unsigned char nal[FRAG_PAYLOAD_SIZE];
    memset(nal, tag, sizeof(nal));
    nal[0] = is_iframe ? 0x65 : 0x41;
    assert(mp4_set_slice((const char *)nal, sizeof(nal), is_iframe) == BUF_OK);
}

static struct Mp4State init_mp4_state(void) {
    struct Mp4State st;
    memset(&st, 0, sizeof(st));
    st.sequence_number = 0;
    st.base_media_decode_time = 0;
    st.default_sample_duration = default_sample_size;
    st.header_sent = true;
    return st;
}

static void test_send_skip_dead_and_state_progression(void) {
    int wfd = -1, rfd = -1;
    make_socketpair(&wfd, &rfd, 4096);

    struct Mp4State st = init_mp4_state();
    const uint64_t duration = st.default_sample_duration;

    stage_fragment(0x10, 1);
    uint32_t seq_before = st.sequence_number;
    uint64_t tfdt_before = st.base_media_decode_time;
    assert(fmp4_send_fragment(wfd, &st) == SOCK_SEND_SENT);
    assert(st.sequence_number == seq_before + 1);
    assert(st.base_media_decode_time == tfdt_before + duration);

    unsigned char sink[1 << 20];
    (void)drain_fd(rfd, sink, sizeof(sink));

    int saw_skip = 0;
    for (int i = 0; i < 256; i++) {
        stage_fragment((unsigned char)(0x20 + i), 0);
        seq_before = st.sequence_number;
        tfdt_before = st.base_media_decode_time;
        enum SockSendResult res = fmp4_send_fragment(wfd, &st);
        assert(st.sequence_number == seq_before + 1);
        assert(st.base_media_decode_time == tfdt_before + duration);
        if (res == SOCK_SEND_SKIPPED) {
            saw_skip = 1;
            break;
        }
        assert(res == SOCK_SEND_SENT);
    }
    assert(saw_skip);

    close(rfd);
    int dead = 0;
    for (int i = 0; i < 8; i++) {
        stage_fragment((unsigned char)(0x40 + i), 0);
        enum SockSendResult res = fmp4_send_fragment(wfd, &st);
        if (res == SOCK_SEND_DEAD) {
            dead = 1;
            break;
        }
    }
    assert(dead);

    close(wfd);
    puts("  send/saturate/dead behavior with timeline advance: OK");
}

static void test_timeline_gap_visible_on_wire_after_skips(void) {
    int wfd = -1, rfd = -1;
    make_socketpair(&wfd, &rfd, 4096);

    struct Mp4State st = init_mp4_state();
    const uint64_t duration = st.default_sample_duration;

    unsigned char wire0[1 << 20];
    stage_fragment(0x80, 1);
    assert(fmp4_send_fragment(wfd, &st) == SOCK_SEND_SENT);
    size_t len0 = drain_fd(rfd, wire0, sizeof(wire0));
    assert(len0 > 0);
    struct MoofFields first = parse_moof_fields_from_wire(wire0, len0);
    assert(first.sequence_number == 0);
    assert(first.base_media_decode_time == 0);

    int skipped = 0;
    for (int i = 0; i < 512 && skipped < TARGET_SKIPS; i++) {
        stage_fragment((unsigned char)(0x90 + i), 0);
        enum SockSendResult res = fmp4_send_fragment(wfd, &st);
        if (res == SOCK_SEND_SKIPPED) {
            skipped++;
            continue;
        }
        assert(res == SOCK_SEND_SENT);
    }
    assert(skipped == TARGET_SKIPS);

    unsigned char sink[1 << 20];
    (void)drain_fd(rfd, sink, sizeof(sink));

    uint32_t expect_seq = st.sequence_number;
    uint64_t expect_tfdt = st.base_media_decode_time;

    stage_fragment(0xb0, 0);
    assert(fmp4_send_fragment(wfd, &st) == SOCK_SEND_SENT);

    unsigned char wire_after[1 << 20];
    size_t after_len = drain_fd(rfd, wire_after, sizeof(wire_after));
    assert(after_len > 0);
    struct MoofFields after = parse_moof_fields_from_wire(wire_after, after_len);

    assert(after.sequence_number == expect_seq);
    assert(after.base_media_decode_time == expect_tfdt);
    assert(after.sequence_number > first.sequence_number);
    assert(after.base_media_decode_time >= duration * (uint64_t)(TARGET_SKIPS + 1));

    close(wfd);
    close(rfd);
    puts("  post-gap moof tfdt includes skipped durations: OK");
}

int main(void) {
    mp4_set_config(1920, 1080, 30, 0, 0, 0, 0);

    test_send_skip_dead_and_state_progression();
    test_timeline_gap_visible_on_wire_after_skips();

    puts("test_fmp4_skip: OK");
    return 0;
}

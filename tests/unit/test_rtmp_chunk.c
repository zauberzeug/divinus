/* Chunk serializer test for rtmp.c: a message whose timestamp needs the
   extended timestamp field (>= 0xFFFFFF) must repeat the 4-byte extended
   timestamp after EVERY type-3 continuation chunk header (Adobe RTMP 1.0,
   5.3.1.3); below the threshold continuation headers stay 1 byte. Includes
   rtmp.c to reach its statics and captures the wire bytes via a socketpair. */

#include "rtmp.c"

#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>

char keepRunning = 1;
unsigned int millis(void) { return 0; }

static int wire_fd = -1;

static void read_exact(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(wire_fd, buf + got, n - got, 0);
        assert(r > 0 && "wire ended early");
        got += (size_t)r;
    }
}

static uint32_t read_u32_be(void) {
    uint8_t b[4];
    read_exact(b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void check_chunked_message(uint32_t timestamp, bool extended) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct timeval tv = {.tv_sec = 2};
    assert(setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    socket_fd = sv[0];
    wire_fd = sv[1];

    uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = i & 0xFF;

    assert(rtmp_send_packet(RTMP_MSG_VIDEO, 1, payload, sizeof(payload), (int)timestamp) == 0);

    uint8_t hdr[12];
    read_exact(hdr, 12);
    assert(hdr[0] == ((0 << 6) | RTMP_CS_VIDEO));
    uint32_t ts_field = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    assert(ts_field == (extended ? 0xFFFFFF : timestamp));
    assert((((uint32_t)hdr[4] << 16) | ((uint32_t)hdr[5] << 8) | hdr[6]) == sizeof(payload));
    assert(hdr[7] == RTMP_MSG_VIDEO);
    if (extended)
        assert(read_u32_be() == timestamp);

    size_t off = 0;
    uint8_t chunk[RTMP_DEFAULT_CHUNK_SIZE];
    while (off < sizeof(payload)) {
        size_t n = sizeof(payload) - off;
        if (n > RTMP_DEFAULT_CHUNK_SIZE) n = RTMP_DEFAULT_CHUNK_SIZE;
        read_exact(chunk, n);
        assert(memcmp(chunk, payload + off, n) == 0);
        off += n;

        if (off < sizeof(payload)) {
            uint8_t basic;
            read_exact(&basic, 1);
            assert(basic == ((3 << 6) | RTMP_CS_VIDEO));
            if (extended)
                assert(read_u32_be() == timestamp);
        }
    }

    socket_fd = -1;
    wire_fd = -1;
    close(sv[0]);
    close(sv[1]);
}

/* The fallback FLV timeline (used when no vendor PTS is available) must derive
   each timestamp as frames*1000/fps, not by adding a truncated 1000/fps per
   frame: 1000/30 = 33 (vs 33.33) drifts ~1% slow, so an hour runs ~36 s short.
   Drive a whole stream and require the end timestamp to land on the exact
   duration. */
static void check_timeline_no_drift(char fps, uint32_t frames, uint32_t expect_ms) {
    flv_set_config(1920, 1080, fps, 0, 0, 0, 0);
    struct FlvState st;
    memset(&st, 0, sizeof(st));
    for (uint32_t i = 0; i < frames; i++)
        assert(flv_inc_timestamp(&st) == BUF_OK);
    uint32_t drift = st.timestamp_ms > expect_ms ? st.timestamp_ms - expect_ms
                                                 : expect_ms - st.timestamp_ms;
    assert(drift <= 1 && "FLV fallback timeline drifts from 1000/fps truncation");
    /* Monotonically non-decreasing along the way (spot-check the midpoint). */
    struct FlvState mid;
    memset(&mid, 0, sizeof(mid));
    for (uint32_t i = 0; i < frames / 2; i++) flv_inc_timestamp(&mid);
    assert(mid.timestamp_ms < st.timestamp_ms);
}

int main(void) {
    check_chunked_message(0x001000, false);
    check_chunked_message(0xFFFFFF, true);
    check_chunked_message(0x01234567, true);
    check_timeline_no_drift(30, 3000, 100000);  /* 3000 frames @30 fps = 100 s exactly */
    check_timeline_no_drift(60, 3000, 50000);   /* 3000 frames @60 fps = 50 s exactly */
    puts("test_rtmp_chunk: OK");
    return 0;
}

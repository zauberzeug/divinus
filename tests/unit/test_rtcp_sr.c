/* RTCP Sender Report sender-info timestamps (RFC 3550 §6.4.1). The pair a
   receiver reads — the NTP wall-clock and the 90 kHz RTP timestamp — must
   describe ONE instant so the RTP timeline maps onto absolute time. The
   redesign splits the two clocks: the NTP field carries the absolute capture
   epoch, while the RTP field rides the monotonic PTS media clock (the same
   clock the per-frame stamps ride), so the receiver's delta runs on the
   NTP-immune media clock and only the anchor carries wall-clock. Both come
   from captime_sr_anchor(pts_anchor, capture_us) — pure and host-testable. */

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* The header web that defines connection_item_t / transport_t and the SR
   emitter, in the same include order src/rtsp/rtp.c uses. */
#include "rtsp/rtsp_server.h"
#include "rtsp/common.h"
#include "rtsp/rtsp.h"
#include "hal/captime.h"
#include "rtsp/rtp.h"
#include "rtsp/rtcp.h"
#include "rtsp/rfc.h"

static void test_rtp_ts_rides_pts_not_capture_epoch(void) {
    /* The load-bearing invariant: SR.rtp_ts is pts_to_rtp90 of the anchor
       frame's PTS (the media clock), and SR.ntp is the absolute capture epoch
       of that SAME frame. The two inputs are independent clocks — the RTP
       field must NOT be re-derived from the capture epoch (the old behavior
       that put a wall-clock-scaled value on the media timeline). */
    unsigned long long pts_anchor = 5000000000ull;        /* 5000 s on the PTS clock */
    unsigned long long capture_us = 1700000000123456ull;  /* 2023-11-14, .123456 s */
    captime_sr_ts sr = captime_sr_anchor(pts_anchor, capture_us);

    /* NTP from the capture epoch: 1900→1970 is 2208988800 s. */
    assert(sr.ntp_sec == 1700000000u + 2208988800u);
    assert(sr.ntp_frac == (unsigned int)((123456ull << 32) / 1000000ull));
    /* RTP-ts is the media clock (PTS), not the epoch. */
    assert(sr.rtp_ts == pts_to_rtp90(pts_anchor));
    assert(sr.rtp_ts != pts_to_rtp90(capture_us) && "must not ride the capture epoch");
}

static void test_receiver_recovers_a_later_frames_capture_time(void) {
    /* The whole point: a receiver maps any frame's RTP timestamp back to its
       absolute capture instant via
           wall(f) = SR.ntp + (f.rtp_ts − SR.rtp_ts) / 90000
       The delta runs on the monotonic media clock (NTP-immune); only SR.ntp is
       wall-clock. A frame 200 ms after the anchor must recover capture+200 ms. */
    unsigned long long pts_anchor = 5000000000ull;
    unsigned long long capture_us = 1700000000123456ull;
    captime_sr_ts sr = captime_sr_anchor(pts_anchor, capture_us);

    unsigned long long pts_frame = pts_anchor + 200000ull;   /* +200 ms on the media clock */
    unsigned int frame_rtp_ts = pts_to_rtp90(pts_frame);     /* what rides this frame's packets */

    /* Receiver side: decode the NTP field back to epoch µs, then add the
       media-clock delta. The delta MUST be a signed 32-bit difference. */
    unsigned long long ntp_us =
        ((unsigned long long)sr.ntp_sec - 2208988800ull) * 1000000ull +
        (((unsigned long long)sr.ntp_frac * 1000000ull) >> 32);
    int delta_ticks = (int)(frame_rtp_ts - sr.rtp_ts);       /* signed: handles wrap & pre-anchor */
    long long delta_us = (long long)delta_ticks * 1000000ll / 90000ll;
    unsigned long long wall_us = ntp_us + (unsigned long long)delta_us;

    /* Recovered capture is the anchor capture + the 200 ms media delta, within
       the ≤1 µs NTP-fraction floor. */
    unsigned long long expected = capture_us + 200000ull;
    assert(wall_us + 1 >= expected && wall_us <= expected + 1);
}

static void test_receiver_delta_is_wrap_correct(void) {
    /* When the anchor's RTP timestamp is just below the 2^32 wrap and a later
       frame's has wrapped past 0, the signed 32-bit delta is still a small
       positive number — a naive unsigned subtract would read ~2^32 and place
       the frame ~13 h in the future. Pins the signed-modular contract. */
    unsigned long long pts_anchor = 47721800000ull;   /* RTP-ts ≈ 4294962000, just below 2^32 */
    unsigned long long capture_us = 1700000000000000ull;
    captime_sr_ts sr = captime_sr_anchor(pts_anchor, capture_us);

    unsigned long long pts_frame = pts_anchor + 100000ull;   /* +100 ms; RTP-ts wraps past 0 */
    unsigned int frame_rtp_ts = pts_to_rtp90(pts_frame);
    assert(frame_rtp_ts < sr.rtp_ts && "the later frame's RTP-ts wrapped below the anchor");

    int delta_ticks = (int)(frame_rtp_ts - sr.rtp_ts);
    assert(delta_ticks == 9000 && "signed delta recovers +100 ms (9000 ticks) across the wrap");
}

static void test_ntp_fraction_fixed_point_scale(void) {
    /* The fraction is 2^-32-unit binary, not decimal µs: a whole second has
       zero fraction, and exactly half a second is the MSB set. Pins the 32.32
       scale so a 1e6-vs-2^32 mix-up can't slip past the round-trip tolerance. */
    assert(captime_sr_anchor(0, 5000000ull).ntp_frac == 0u);
    assert(captime_sr_anchor(0, 5500000ull).ntp_frac == 0x80000000u);
}

static void test_sr_wire_size_excludes_report_blocks(void) {
    /* An SR with RC=0 is the 4-byte common header + 24-byte sender info = 28
       bytes; the header length field is words-minus-one = 6. The old code sent
       36 B / length 8, trailing 8 uninitialised bytes from the rr[1] block.
       Tie the constants to the struct so the regression can't return. */
    assert(RTCP_SR_NORB_BYTES == offsetof(rtcp_t, r.sr.rr));
    assert(RTCP_SR_NORB_BYTES == 28u);
    assert(RTCP_SR_NORB_LENGTH == RTCP_SR_NORB_BYTES / 4u - 1u);
    assert(RTCP_SR_NORB_LENGTH == 6u);
}

/* Loopback UDP pair: a connected sender fd (stands in for server_rtcp_fd, which
   production connect()s at SETUP) and a bound, read-timed receiver fd. */
static void udp_pair(int *server_fd, int *recv_fd) {
    int rfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(rfd >= 0);
    struct sockaddr_in raddr = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(rfd, (struct sockaddr *)&raddr, sizeof(raddr)));
    socklen_t alen = sizeof(raddr);
    assert(!getsockname(rfd, (struct sockaddr *)&raddr, &alen));
    struct timeval tv = {.tv_sec = 2};
    assert(!setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));

    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sfd >= 0);
    assert(!connect(sfd, (struct sockaddr *)&raddr, sizeof(raddr)));

    *server_fd = sfd;
    *recv_fd = rfd;
}

/* Receive one SR datagram and read its cumulative packet/octet counters
   (network order) from the fixed sender-info offsets. */
static void recv_sr_counts(int rfd, unsigned int *psent, unsigned int *osent) {
    unsigned char buf[64];
    int n = recvfrom(rfd, buf, sizeof(buf), 0, NULL, NULL);
    assert(n == (int)RTCP_SR_NORB_BYTES && "expected a 28-byte no-RB sender report");
    *psent = ((unsigned)buf[20] << 24) | ((unsigned)buf[21] << 16) |
             ((unsigned)buf[22] << 8) | buf[23];
    *osent = ((unsigned)buf[24] << 24) | ((unsigned)buf[25] << 16) |
             ((unsigned)buf[26] << 8) | buf[27];
}

static void test_sr_counts_are_cumulative_across_reports(void) {
    /* RFC 3550 §6.4.1: psent/osent are totals since transmission start, so two
       consecutive SRs (with more packets sent in between) must carry the
       running totals — not per-interval counts. A receiver differencing the two
       recovers the interval; resetting the counters per SR breaks that math. */
    int sfd, rfd;
    udp_pair(&sfd, &rfd);

    struct connection_item_t con;
    memset(&con, 0, sizeof(con));
    con.ssrc = 0x1234abcd;
    assert(!pthread_mutex_init(&con.write_mutex, NULL));

    transport_t *t = &con.trans[0];
    t->is_tcp = 0;                 /* exercise the UDP send() path */
    t->server_rtcp_fd = sfd;
    t->rtcp_tick_org = 90;
    t->capture_us = 0;             /* no capture time: send-time NTP/RTP pair */

    /* First interval: 10 packets / 12000 octets accumulated, then an SR. */
    t->rtcp_packet_cnt = 10;
    t->rtcp_octet = 12000;
    assert(__rtcp_send_sr(&con, 0) == SUCCESS);
    unsigned int p1, o1;
    recv_sr_counts(rfd, &p1, &o1);
    assert(p1 == 10 && o1 == 12000);

    /* The SR must leave the counters intact (this is the regression the fix
       guards): only the SR-interval timer resets. */
    assert(t->rtcp_packet_cnt == 10 && t->rtcp_octet == 12000 &&
        "SR emission must not reset the cumulative counters");
    assert(t->rtcp_tick == t->rtcp_tick_org && "SR resets only the interval timer");

    /* Second interval: 15 more packets / 18000 more octets, then a second SR. */
    t->rtcp_packet_cnt += 15;
    t->rtcp_octet += 18000;
    assert(__rtcp_send_sr(&con, 0) == SUCCESS);
    unsigned int p2, o2;
    recv_sr_counts(rfd, &p2, &o2);

    /* Cumulative totals, strictly increasing across the two reports. */
    assert(p2 == 25 && o2 == 30000);
    assert(p2 > p1 && o2 > o1);

    pthread_mutex_destroy(&con.write_mutex);
    close(sfd);
    close(rfd);
}

int main(void) {
    test_rtp_ts_rides_pts_not_capture_epoch();
    test_receiver_recovers_a_later_frames_capture_time();
    test_receiver_delta_is_wrap_correct();
    test_ntp_fraction_fixed_point_scale();
    test_sr_wire_size_excludes_report_blocks();
    test_sr_counts_are_cumulative_across_reports();
    puts("test_rtcp_sr: OK");
    return 0;
}

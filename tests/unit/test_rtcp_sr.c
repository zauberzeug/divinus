/* RTCP Sender Report sender-info timestamps (RFC 3550 §6.4.1). The pair a
   receiver reads — the NTP wall-clock and the 90 kHz RTP timestamp — must
   describe ONE instant so the RTP timeline maps onto absolute time. The
   redesign splits the two clocks: the NTP field carries the absolute capture
   epoch, while the RTP field rides the monotonic PTS media clock (the same
   clock the per-frame stamps ride), so the receiver's delta runs on the
   NTP-immune media clock and only the anchor carries wall-clock. Both come
   from captime_sr_anchor(pts_anchor, capture_us) — pure and host-testable. */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "hal/captime.h"
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

int main(void) {
    test_rtp_ts_rides_pts_not_capture_epoch();
    test_receiver_recovers_a_later_frames_capture_time();
    test_receiver_delta_is_wrap_correct();
    test_ntp_fraction_fixed_point_scale();
    test_sr_wire_size_excludes_report_blocks();
    puts("test_rtcp_sr: OK");
    return 0;
}

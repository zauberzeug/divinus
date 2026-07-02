/* Capture-time helper: rebases a vendor frame PTS onto the NTP-disciplined
   wall clock to recover the absolute epoch-µs instant a frame was captured.
   Pure and host-testable — the two OS clocks and the PTS are injected, never
   read inline, so the conversion math is exercised without a camera. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hal/captime.h"

static void test_identity_epoch_recovers_capture_age(void) {
    /* PTS already on the CLOCK_MONOTONIC_RAW timeline (offset 0): a frame
       captured 30 ms ago must map to wall-clock-now minus 30 ms. */
    captime_calib cal = { .pts_minus_mono_us = 0, .max_frame_age_us = 5000000ull };
    unsigned long long mono_now = 1000000000ull;          /* arbitrary monotonic µs */
    unsigned long long pts = mono_now - 30000ull;         /* 30 ms old */
    unsigned long long real_now = 1700000000000000ull;    /* epoch µs */
    unsigned long long capture = 0;

    assert(captime_from_pts(pts, mono_now, real_now, &cal, &capture));
    assert(capture == real_now - 30000ull);
}

static void test_vendor_clock_ahead_no_silent_fallback(void) {
    /* The regression the PRs' `pts <= mono_us` guard caused: the vendor clock
       runs ~430 ms AHEAD of MONOTONIC_RAW, so a freshly captured frame has
       pts > mono_now and the old guard silently fell back to send-time. With
       the measured offset the rebase must still succeed and be correct. */
    captime_calib cal = { .pts_minus_mono_us = 430000ll, .max_frame_age_us = 5000000ull };
    unsigned long long mono_now = 1000000000ull;
    /* frame captured 30 ms ago: vendor PTS = (mono_now − 30ms) + 430ms ahead */
    unsigned long long pts = mono_now - 30000ull + 430000ull;
    unsigned long long real_now = 1700000000000000ull;
    unsigned long long capture = 0;

    assert(pts > mono_now);  /* the exact input the old guard rejected */
    assert(captime_from_pts(pts, mono_now, real_now, &cal, &capture));
    assert(capture == real_now - 30000ull);
}

static void test_implausible_age_rejected_without_writing(void) {
    /* When the rebased age is not physically possible — a stale/garbage PTS
       older than the ceiling, or a PTS that lands in the future — the helper
       must refuse rather than fake a value. The caller then omits the stamp;
       it never silently substitutes an unrelated send-time. */
    captime_calib cal = { .pts_minus_mono_us = 0, .max_frame_age_us = 5000000ull };
    unsigned long long mono_now = 1000000000ull;
    unsigned long long real_now = 1700000000000000ull;
    unsigned long long capture = 0xDEADBEEFull;  /* sentinel must survive */

    /* 10 s old, ceiling is 5 s */
    assert(!captime_from_pts(mono_now - 10000000ull, mono_now, real_now, &cal, &capture));
    assert(capture == 0xDEADBEEFull);

    /* PTS in the future (negative age) */
    assert(!captime_from_pts(mono_now + 100000ull, mono_now, real_now, &cal, &capture));
    assert(capture == 0xDEADBEEFull);
}

static void test_vendor_clock_behind_is_symmetric(void) {
    /* §9.1's sign is not yet measured: the offset may be negative (vendor
       clock behind MONOTONIC_RAW). The signed math must be correct either
       way, so the measured number can be dropped in without a code change. */
    captime_calib cal = { .pts_minus_mono_us = -430000ll, .max_frame_age_us = 5000000ull };
    unsigned long long mono_now = 1000000000ull;
    unsigned long long pts = mono_now - 30000ull - 430000ull;  /* 30 ms old, clock behind */
    unsigned long long real_now = 1700000000000000ull;
    unsigned long long capture = 0;

    assert(captime_from_pts(pts, mono_now, real_now, &cal, &capture));
    assert(capture == real_now - 30000ull);
}

static void test_no_32bit_wrap_at_long_uptime(void) {
    /* millis() is u32 ms and wraps every ~49.7 days; that is precisely the
       pitfall this helper avoids by staying 64-bit µs throughout. At ~58 days
       of uptime (well past the u32-ms wrap) the rebase is still exact. */
    unsigned long long past_u32_ms_wrap = 5000000000000ull;  /* ≈ 57.9 days in µs */
    captime_calib cal = { .pts_minus_mono_us = 0, .max_frame_age_us = 5000000ull };
    unsigned long long pts = past_u32_ms_wrap - 30000ull;
    unsigned long long real_now = 1700000000000000ull;
    unsigned long long capture = 0;

    assert(captime_from_pts(pts, past_u32_ms_wrap, real_now, &cal, &capture));
    assert(capture == real_now - 30000ull);
}

static void test_zero_age_maps_to_now(void) {
    /* A frame whose PTS is exactly "now" maps to wall-clock-now. */
    captime_calib cal = { .pts_minus_mono_us = 0, .max_frame_age_us = 5000000ull };
    unsigned long long now = 1000000000ull, real_now = 1700000000000000ull, capture = 0;

    assert(captime_from_pts(now, now, real_now, &cal, &capture));
    assert(capture == real_now);
}

static void test_age_ceiling_is_inclusive_boundary(void) {
    /* The ceiling itself is accepted; one µs past it is rejected. */
    captime_calib cal = { .pts_minus_mono_us = 0, .max_frame_age_us = 1000000ull };
    unsigned long long mono_now = 5000000000ull, real_now = 1700000000000000ull, capture = 0;

    assert(captime_from_pts(mono_now - cal.max_frame_age_us, mono_now, real_now, &cal, &capture));
    assert(capture == real_now - cal.max_frame_age_us);
    assert(!captime_from_pts(mono_now - cal.max_frame_age_us - 1, mono_now, real_now, &cal, &capture));
}

static void test_pts_to_rtp90_scale_and_wrap(void) {
    /* The per-frame RTP timestamp rides a 90 kHz clock fed by the vendor PTS
       (the monotonic media clock), NOT the capture epoch. The mapping is the
       single one shared by the RTSP and UDP packetizers, so pin its scale and
       its 32-bit wrap here: 1 ms of PTS is 90 ticks, 1 s is 90000, and the
       value truncates to 32 bits (wraps every 2^32/90000 ≈ 13.25 h). */
    assert(pts_to_rtp90(0) == 0u);
    assert(pts_to_rtp90(1000ull) == 90u);           /* 1 ms -> 90 ticks */
    assert(pts_to_rtp90(1000000ull) == 90000u);     /* 1 s  -> 90000 ticks */

    /* Wrap: past 2^32 ticks (≈13.9 h of PTS) the result is the low 32 bits of
       the full 64-bit product — modular, never a saturated/overspilled value. */
    unsigned long long big_pts = 50000000000ull;        /* 50000 s, past the 13.25 h wrap */
    unsigned long long full = big_pts * 90ull / 1000ull; /* > 2^32 */
    assert(full > (1ull << 32));
    assert(pts_to_rtp90(big_pts) == (unsigned int)full);
}

static void test_rtp_ts_advance_resolves_millis_seed_pin(void) {
    /* The send path seeds the wire timestamp from millis()*90 on an early frame
       (before capture is ready), then advances it per AU. The advance guard is
       a signed 32-bit compare: adopt `next` if it is genuinely ahead, else
       prev+1 so a duplicate source still moves forward by one tick.

       The bug the redesign fixes: when `next` was the capture EPOCH mod 2^32
       (~1.23e9) it sat numerically BELOW the millis()*90 seed (~1.476e9), so
       every frame "non-advanced" and the wire timestamp crawled +1 forever. */
    unsigned int millis_seed = 1476694767u;   /* ≈ uptime·90000 at ~16417 s */
    unsigned int epoch_mod   = 1230107166u;   /* capture epoch µs ·90/1000 mod 2^32 */
    assert((int)(epoch_mod - millis_seed) < 0);                 /* the pin condition */
    assert(rtp_ts_advance(millis_seed, epoch_mod) == millis_seed + 1u);  /* the +1 crawl */

    /* Resolved: feeding pts_to_rtp90(pts) values — same magnitude as the seed
       (both ≈ uptime·90000) and monotonic — the guard adopts the real value. */
    unsigned int pts_next = millis_seed + 450u;   /* a few frames later on the media clock */
    assert(rtp_ts_advance(millis_seed, pts_next) == pts_next);  /* adopts, no pin */

    /* A genuine duplicate PTS (no advance) still nudges +1 so the wire timeline
       never stalls — the only case the guard fires under the PTS clock. */
    assert(rtp_ts_advance(pts_next, pts_next) == pts_next + 1u);
}

static void test_rtp_ts_advance_is_wrap_correct(void) {
    /* Across the 2^32 wrap (~13.25 h) the signed compare keeps advancing: a
       small forward step that straddles the wrap is adopted, while a large
       backward jump (a real non-advance) is clamped to prev+1. */
    unsigned int before_wrap = 0xFFFFFF00u;
    unsigned int after_wrap  = 0x00000100u;   /* +0x200 ticks, wrapped */
    assert((int)(after_wrap - before_wrap) > 0);
    assert(rtp_ts_advance(before_wrap, after_wrap) == after_wrap);   /* adopt across wrap */

    /* Backward (would rewind the timeline) -> clamp to prev+1, with wrap. */
    assert(rtp_ts_advance(0x00000100u, 0xFFFFFF00u) == 0x00000101u);
    assert(rtp_ts_advance(0xFFFFFFFFu, 0xFFFFFFFFu) == 0u);          /* prev+1 wraps to 0 */
}

static void test_ntp_be64_matches_sr_and_round_trips(void) {
    /* The per-frame abs-capture-time RTP header extension carries the capture
       instant as an 8-byte big-endian Q32.32 NTP timestamp (1900 epoch). It
       MUST encode the exact same NTP value the RTCP SR puts on the wire, so a
       receiver gets one consistent absolute time whichever carrier it reads. */
    unsigned long long capture_us = 1700000000123456ull;
    unsigned char b[8];
    captime_ntp_be64(b, capture_us);

    unsigned int ntp_sec  = ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) |
                            ((unsigned)b[2] << 8)  | b[3];
    unsigned int ntp_frac = ((unsigned)b[4] << 24) | ((unsigned)b[5] << 16) |
                            ((unsigned)b[6] << 8)  | b[7];
    captime_sr_ts sr = captime_sr_anchor(0, capture_us);
    assert(ntp_sec == sr.ntp_sec && ntp_frac == sr.ntp_frac);

    /* Round-trips to the capture instant within the ≤1 µs fraction floor. */
    unsigned long long decoded =
        ((unsigned long long)ntp_sec - 2208988800ull) * 1000000ull +
        (((unsigned long long)ntp_frac * 1000000ull) >> 32);
    assert(decoded + 1 >= capture_us && decoded <= capture_us);
}

static void test_abs_capture_ext_one_byte_framing(void) {
    /* RFC 8285 §4.2 one-byte header extension carrying abs-capture-time:
       the 0xBEDE profile marker, a 16-bit length in 32-bit words, one element
       (4-bit id, 4-bit len = bytes−1) with the 8-byte Q32.32 payload, padded
       with zero bytes to the next 32-bit boundary. 1+8 = 9 data bytes pad to
       12 = 3 words, so the whole block is 2+2+12 = 16 bytes. */
    unsigned long long capture_us = 1700000000123456ull;
    unsigned char ext[32];
    memset(ext, 0xEE, sizeof(ext));  /* poison so padding zeros are proven written */
    int n = captime_abs_capture_ext(ext, CAPTIME_ABS_CAPTURE_EXT_ID, capture_us);

    assert(n == 16);
    assert(ext[0] == 0xBE && ext[1] == 0xDE);          /* one-byte profile marker */
    assert(ext[2] == 0 && ext[3] == 3);                /* length = 3 words of data */
    assert(ext[4] == ((CAPTIME_ABS_CAPTURE_EXT_ID << 4) | 7));  /* id, len nibble = 8−1 */

    unsigned char payload[8];
    captime_ntp_be64(payload, capture_us);
    assert(!memcmp(ext + 5, payload, 8) && "element payload is the Q32.32 NTP time");
    assert(ext[13] == 0 && ext[14] == 0 && ext[15] == 0 && "padded to a 32-bit boundary");
}

static void test_format_secs_usec(void) {
    /* The wire form shared by the stream timestamps: whole seconds, a dot,
       and exactly 6 zero-padded microsecond digits — parseable as a double
       and losslessly back to µs. */
    char buf[32];
    int n = captime_format(buf, sizeof(buf), 1700000000123456ull);

    assert(n == 17);
    assert(!strcmp(buf, "1700000000.123456"));

    /* sub-second value keeps the leading zeros */
    captime_format(buf, sizeof(buf), 7000042ull);
    assert(!strcmp(buf, "7.000042"));
}

int main(void) {
    test_identity_epoch_recovers_capture_age();
    test_vendor_clock_ahead_no_silent_fallback();
    test_implausible_age_rejected_without_writing();
    test_vendor_clock_behind_is_symmetric();
    test_no_32bit_wrap_at_long_uptime();
    test_zero_age_maps_to_now();
    test_age_ceiling_is_inclusive_boundary();
    test_pts_to_rtp90_scale_and_wrap();
    test_rtp_ts_advance_resolves_millis_seed_pin();
    test_rtp_ts_advance_is_wrap_correct();
    test_ntp_be64_matches_sr_and_round_trips();
    test_abs_capture_ext_one_byte_framing();
    test_format_secs_usec();
    puts("test_captime: OK");
    return 0;
}

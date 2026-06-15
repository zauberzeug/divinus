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
    test_format_secs_usec();
    puts("test_captime: OK");
    return 0;
}

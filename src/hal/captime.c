#include <stdio.h>
#include <time.h>

#include "captime.h"

bool captime_from_pts(unsigned long long pts_us, unsigned long long mono_raw_now_us,
                      unsigned long long real_now_us, const captime_calib *cal,
                      unsigned long long *out_capture_us)
{
    long long pts_on_mono = (long long)pts_us - cal->pts_minus_mono_us;
    long long frame_age = (long long)mono_raw_now_us - pts_on_mono;

    if (frame_age < 0 || (unsigned long long)frame_age > cal->max_frame_age_us)
        return false;

    *out_capture_us = real_now_us - (unsigned long long)frame_age;
    return true;
}

static unsigned long long ts_to_us(const struct timespec *ts)
{
    return (unsigned long long)ts->tv_sec * 1000000ull + ts->tv_nsec / 1000ull;
}

bool captime_now(unsigned long long pts_us, unsigned long long *out_capture_us)
{
    static const captime_calib cal = {
        .pts_minus_mono_us = CAPTIME_PTS_MONO_OFFSET_US,
        .max_frame_age_us  = CAPTIME_MAX_FRAME_AGE_US,
    };
    struct timespec mono, real;

    /* MONOTONIC_RAW (immune to NTP slew) then REALTIME, back-to-back: the
       residual inter-read skew is sub-µs (two adjacent syscalls), well inside
       the §8 error budget, so the read order is not significant. */
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &mono) != 0 ||
        clock_gettime(CLOCK_REALTIME, &real) != 0)
        return false;

    return captime_from_pts(pts_us, ts_to_us(&mono), ts_to_us(&real), &cal,
                            out_capture_us);
}

int captime_format(char *buf, unsigned long buf_size, unsigned long long capture_us)
{
    return snprintf(buf, buf_size, "%llu.%06llu",
                    capture_us / 1000000ull, capture_us % 1000000ull);
}

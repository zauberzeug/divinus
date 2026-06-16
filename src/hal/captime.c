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

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01). */
#define NTP_UNIX_EPOCH_OFFSET 2208988800ull

unsigned int pts_to_rtp90(unsigned long long pts_us)
{
    return (unsigned int)(pts_us * 90ull / 1000ull);
}

unsigned int rtp_ts_advance(unsigned int prev, unsigned int next)
{
    return (int)(next - prev) > 0 ? next : prev + 1;
}

static void ntp_from_capture(unsigned long long capture_us,
                             unsigned int *ntp_sec, unsigned int *ntp_frac)
{
    unsigned long long sec  = capture_us / 1000000ull;
    unsigned long long usec = capture_us % 1000000ull;

    *ntp_sec  = (unsigned int)(sec + NTP_UNIX_EPOCH_OFFSET);
    *ntp_frac = (unsigned int)((usec << 32) / 1000000ull);
}

captime_sr_ts captime_sr_anchor(unsigned long long pts_anchor_us,
                                unsigned long long capture_us)
{
    captime_sr_ts sr;

    ntp_from_capture(capture_us, &sr.ntp_sec, &sr.ntp_frac);
    sr.rtp_ts = pts_to_rtp90(pts_anchor_us);
    return sr;
}

void captime_ntp_be64(unsigned char out[8], unsigned long long capture_us)
{
    unsigned int ntp_sec, ntp_frac;

    ntp_from_capture(capture_us, &ntp_sec, &ntp_frac);
    out[0] = ntp_sec >> 24;  out[1] = ntp_sec >> 16;
    out[2] = ntp_sec >> 8;   out[3] = ntp_sec;
    out[4] = ntp_frac >> 24; out[5] = ntp_frac >> 16;
    out[6] = ntp_frac >> 8;  out[7] = ntp_frac;
}

int captime_abs_capture_ext(unsigned char *dst, unsigned int ext_id,
                            unsigned long long capture_us)
{
    dst[0] = 0xBE;  /* one-byte header extension profile marker (RFC 8285) */
    dst[1] = 0xDE;
    dst[2] = 0x00;  /* extension data length = 3 32-bit words (1 elem + pad) */
    dst[3] = 0x03;
    dst[4] = (unsigned char)((ext_id << 4) | 7);  /* id, len nibble = 8 bytes − 1 */
    captime_ntp_be64(&dst[5], capture_us);
    dst[13] = dst[14] = dst[15] = 0;              /* pad to the 32-bit boundary */
    return CAPTIME_ABS_CAPTURE_EXT_BYTES;
}

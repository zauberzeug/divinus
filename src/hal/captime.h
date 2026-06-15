#pragma once

#include <stdbool.h>

/* Calibration relating the vendor frame PTS clock to the OS clocks. Both
   fields are facts measured on the camera (§9.1 of the timing-stack packet),
   injected here so the conversion stays pure and host-testable. */
typedef struct {
    /* (vendor PTS) − (CLOCK_MONOTONIC_RAW) sampled at one instant, µs.
       Signed: positive means the vendor clock runs ahead of MONOTONIC_RAW.
       0 when the PTS already shares the MONOTONIC_RAW epoch. */
    long long pts_minus_mono_us;
    /* Plausibility ceiling for the rebased frame age. A rebase older than
       this is rejected (returns false) rather than silently faked. */
    unsigned long long max_frame_age_us;
} captime_calib;

/* Recover the absolute epoch-µs instant a frame was captured from its vendor
   PTS and a back-to-back reading of CLOCK_MONOTONIC_RAW + CLOCK_REALTIME:

     capture = real_now − (mono_raw_now − (pts − pts_minus_mono_us))

   The monotonic delta is the true elapsed time since capture (immune to NTP
   steps); real_now is the absolute anchor (read fresh so NTP corrections are
   tracked). Writes *out_capture_us and returns true on success; returns false
   without touching *out_capture_us when the rebased age is implausible. */
bool captime_from_pts(unsigned long long pts_us, unsigned long long mono_raw_now_us,
                      unsigned long long real_now_us, const captime_calib *cal,
                      unsigned long long *out_capture_us);

/* Measured on zz-cam-005 (§9.1, 2026-06-15, SSC30KQ+IMX335): the SigmaStar
   MI_SYS PTS clock is CLOCK_MONOTONIC_RAW to within ~7 µs, so the offset is 0.
   (The PRs' "~430 ms ahead" was the slewed CLOCK_MONOTONIC, which diverges
   from RAW with uptime — hence RAW here.) The age ceiling backstops garbage:
   the PTS-to-callback age is pipeline-bound (measured 5–17 ms, near-independent
   of frame rate), so 1 s comfortably clears the slowest config (3 fps floor)
   plus startup transients while still rejecting a stale/uninitialised PTS or a
   gross miscalibration. */
#define CAPTIME_PTS_MONO_OFFSET_US 0LL
#define CAPTIME_MAX_FRAME_AGE_US   1000000ull

/* Production entry point: reads CLOCK_MONOTONIC_RAW + CLOCK_REALTIME
   back-to-back and rebases pts_us with the measured default calibration.
   Returns true and writes *out_capture_us on success; false (output
   untouched) when the clocks are unreadable or the rebase is implausible —
   the caller then omits the timestamp rather than emit a wrong one. */
bool captime_now(unsigned long long pts_us, unsigned long long *out_capture_us);

/* Format an epoch-µs instant as the canonical "<seconds>.<6-digit µs>" wire
   form shared by the stream timestamps (e.g. MJPEG X-Timestamp). Writes into
   buf (snprintf semantics) and returns the character count. */
int captime_format(char *buf, unsigned long buf_size, unsigned long long capture_us);

/* Map a capture instant (epoch µs) to its 90 kHz RTP timestamp, truncated to
   32 bits. The single mapping shared by the per-frame RTP stamp (rtp.c) and the
   RTCP Sender Report, so a frame's RTP timestamp and the SR that anchors it
   agree by construction. Wraps every 2^32 / 90000 ≈ 13.25 h; the receiver
   tracks wraps through the SR cadence. */
unsigned int captime_to_rtp90(unsigned long long capture_us);

/* The sender-info timestamps of an RTCP Sender Report (RFC 3550 §6.4.1), all
   derived from one capture instant so the NTP wall-clock and the RTP timeline
   it anchors describe the SAME instant. Host byte order; the caller applies
   htonl when writing the packet. */
typedef struct {
    unsigned int ntp_sec;   /* seconds since 1900-01-01 (the NTP epoch) */
    unsigned int ntp_frac;  /* binary fraction of a second, 2^-32 units */
    unsigned int rtp_ts;    /* 90 kHz RTP timestamp of that same instant */
} captime_sr_ts;

/* Map a capture instant (epoch µs) to the SR sender-info timestamps above. */
captime_sr_ts captime_sr_from_capture(unsigned long long capture_us);

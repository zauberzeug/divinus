#pragma once

#include <stdbool.h>

/* Calibration relating the vendor frame PTS clock to the OS clocks. Both
   fields are facts measured on the camera, injected here so the conversion
   stays pure and host-testable. */
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

/* Measured on zz-cam-005 (2026-06-15, SSC30KQ+IMX335): the SigmaStar
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

/* Map a vendor PTS instant (µs on the monotonic media clock) to its 90 kHz RTP
   timestamp, truncated to 32 bits. This is the media clock the per-frame RTP
   stamp rides — NOT the capture epoch — so the timeline is monotonic by
   construction (the PTS is CLOCK_MONOTONIC_RAW). The single mapping shared by
   the RTSP packetizer (rtp.c), the UDP push packetizer (stream.c), and the
   RTCP SR's rtp_ts, so every RTP timestamp a receiver sees agrees by
   construction. Wraps every 2^32 / 90000 ≈ 13.25 h; the receiver tracks wraps
   through the signed 32-bit delta against the SR anchor. */
unsigned int pts_to_rtp90(unsigned long long pts_us);

/* Advance a strictly-increasing 32-bit RTP timestamp from prev to next, the
   monotonic guard shared by both packetizers. Returns next when it is genuinely
   ahead of prev (signed 32-bit modular compare, so it is wrap-correct), else
   prev + 1 so a duplicate/non-advancing source still moves the timeline forward
   by one tick. Feeding it pts_to_rtp90(pts) values keeps it harmless: the PTS
   is monotonic, so it only ever fires on a genuine duplicate PTS. (Feeding it
   the capture EPOCH was the bug — a stale millis()*90 seed sat numerically
   above the epoch-mod-2^32 value, so every frame "non-advanced" and the wire
   timestamp crawled +1 forever near the millis origin.) */
unsigned int rtp_ts_advance(unsigned int prev, unsigned int next);

/* The sender-info timestamps of an RTCP Sender Report (RFC 3550 §6.4.1). The
   NTP wall-clock and the RTP timestamp describe the SAME anchor frame, but on
   two clocks: NTP carries the absolute capture epoch, RTP-ts rides the PTS
   media clock (so a receiver's RTP delta is NTP-immune). Host byte order; the
   caller applies htonl when writing the packet. */
typedef struct {
    unsigned int ntp_sec;   /* seconds since 1900-01-01 (the NTP epoch) */
    unsigned int ntp_frac;  /* binary fraction of a second, 2^-32 units */
    unsigned int rtp_ts;    /* 90 kHz PTS-media-clock timestamp of that frame */
} captime_sr_ts;

/* Build the SR sender-info timestamps for one anchor frame: rtp_ts =
   pts_to_rtp90(pts_anchor_us) (the media clock the frames ride), ntp = the
   absolute capture epoch capture_us. Passing both from the same frame keeps
   the (NTP, RTP-ts) pair describing one instant by construction. */
captime_sr_ts captime_sr_anchor(unsigned long long pts_anchor_us,
                                unsigned long long capture_us);

/* Encode a capture instant (epoch µs) as an 8-byte big-endian Q32.32 NTP
   timestamp (seconds since 1900 in the high word, binary fraction in the low),
   the same value the RTCP SR carries. This is the payload of the per-frame
   abs-capture-time RTP header extension; the single encoder shared by the SR
   and the extension so both describe a frame with one consistent absolute time. */
void captime_ntp_be64(unsigned char out[8], unsigned long long capture_us);

/* The WebRTC abs-capture-time RTP header extension id advertised in the SDP
   (a=extmap) and written into the one-byte header on the wire; sender and
   receiver must agree on it. 1–14 is the valid one-byte range (RFC 8285 §4.2).
   The full extension block is the 0xBEDE profile word + a 1-word length + the
   element (id/len byte + 8-byte payload) padded to a 32-bit boundary. */
#define CAPTIME_ABS_CAPTURE_EXT_ID    10u
#define CAPTIME_ABS_CAPTURE_EXT_BYTES 16

/* Write the complete abs-capture-time one-byte RTP header extension block
   (RFC 8285 §4.2; WebRTC abs-capture-time, offset field omitted) for capture_us
   into dst, which must hold at least CAPTIME_ABS_CAPTURE_EXT_BYTES bytes.
   Returns the number of bytes written. ext_id must be in 1..14. */
int captime_abs_capture_ext(unsigned char *dst, unsigned int ext_id,
                            unsigned long long capture_us);

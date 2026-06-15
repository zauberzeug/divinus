/* RTCP Sender Report sender-info timestamps: the NTP wall-clock and the 90 kHz
   RTP timestamp must describe the SAME capture instant (RFC 3550 §6.4.1) so a
   receiver can map the RTP timeline onto absolute time. Both are derived here
   from one injected capture instant — pure and host-testable, no socket. */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "hal/captime.h"
#include "rtsp/rfc.h"

static void test_fields_derive_from_one_instant(void) {
    /* Tracer bullet: a known epoch-µs instant must yield the 1900-epoch NTP
       seconds, the 32.32 fraction, and the 90 kHz RTP timestamp that the
       per-frame stamp would carry — all three from the single input, so the
       NTP↔RTP-ts pair cannot drift apart the way the gettimeofday()-vs-last-
       frame code did. */
    unsigned long long capture_us = 1700000000123456ull;  /* 2023-11-14, .123456 s */
    captime_sr_ts sr = captime_sr_from_capture(capture_us);

    /* 1900→1970 is 2208988800 s; NTP seconds = epoch seconds + that offset. */
    assert(sr.ntp_sec == 1700000000u + 2208988800u);
    /* fraction = floor(usec / 1e6 · 2^32) */
    assert(sr.ntp_frac == (unsigned int)((123456ull << 32) / 1000000ull));
    /* RTP-ts is the shared 90 kHz mapping, not a re-derivation */
    assert(sr.rtp_ts == captime_to_rtp90(capture_us));
}

static void test_ntp_roundtrips_to_capture_instant(void) {
    /* The receiver reads the NTP field back to wall-clock time. Decoding the
       32.32 fixed-point must recover the capture instant to within its sub-µs
       quantization (2^-32 s ≈ 233 ps), proving the field genuinely carries the
       capture time and not send-time now(). */
    unsigned long long capture_us = 1700000000999999ull;  /* worst-case .999999 */
    captime_sr_ts sr = captime_sr_from_capture(capture_us);

    unsigned long long sec  = (unsigned long long)sr.ntp_sec - 2208988800ull;
    unsigned long long usec = ((unsigned long long)sr.ntp_frac * 1000000ull) >> 32;
    unsigned long long decoded = sec * 1000000ull + usec;

    assert(decoded + 1 >= capture_us && decoded <= capture_us);  /* ≤1 µs floor loss */
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

static void test_ntp_fraction_fixed_point_scale(void) {
    /* The fraction is 2^-32-unit binary, not decimal µs: a whole second has
       zero fraction, and exactly half a second is the MSB set. Pins the 32.32
       scale so a 1e6-vs-2^32 mix-up can't slip past the round-trip tolerance. */
    assert(captime_sr_from_capture(5000000ull).ntp_frac == 0u);
    assert(captime_sr_from_capture(5500000ull).ntp_frac == 0x80000000u);
}

int main(void) {
    test_fields_derive_from_one_instant();
    test_ntp_roundtrips_to_capture_instant();
    test_ntp_fraction_fixed_point_scale();
    test_sr_wire_size_excludes_report_blocks();
    puts("test_rtcp_sr: OK");
    return 0;
}

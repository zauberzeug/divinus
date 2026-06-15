#pragma once

#include <stddef.h>

/* Shared H.26x-over-RTP packetization, used by both senders:
   the RTSP server (src/rtsp/rtp.c) and the raw UDP push stream (src/stream.c).
   Keeping the Annex-B split and the RFC 6184 / RFC 7798 FU framing in one place
   means a protocol fix lands on both transports at once. Transport-specific
   concerns (per-destination sequence/timestamp/SSRC stamping and the actual
   socket send) stay in each sender via the emit callback. */

/* Annex-B NAL iterator. Initialize *nalptr = buf and *p_len = 0, then call
   repeatedly: each call advances to the next NAL unit, pointing *nalptr at its
   bare bytes (start code stripped) and setting *p_len to its length (trailing
   start-code zero bytes trimmed). Returns 0 while a NAL was produced, -1 once
   the pack is exhausted. Only 4-byte (00 00 00 01) start codes are recognized,
   matching the encoders both senders are fed. */
int nal_next(unsigned char *buf, unsigned char **nalptr, size_t *p_len, size_t max_len);

/* Emits one RTP packet body: hdr_len header bytes (the FU indicator/header,
   0 for a single-NAL packet) followed by body_len bytes at body. The caller
   prepends the 12-byte RTP header and sends; marker is that packet's RTP
   marker bit. Return 0 to continue, non-zero to abort the NAL. */
typedef int (*rtp_emit_fn)(void *ctx, const unsigned char *hdr, int hdr_len,
                           const unsigned char *body, int body_len, int marker);

/* Packetizes one bare NAL unit (no start code) per RFC 6184 (H.264) /
   RFC 7798 (H.265): a single packet when it fits in max_payload (RTP payload
   bytes, i.e. excluding the 12-byte header), else a run of FU fragments.
   marker_au_end is the marker bit for the NAL's final packet — pass 1 only when
   this NAL ends the access unit. Returns the first non-zero emit() result, else 0. */
int rtp_packetize_nal(const unsigned char *nal, int nal_len, int is_h265,
                      int max_payload, int marker_au_end,
                      rtp_emit_fn emit, void *ctx);

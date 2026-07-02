#include "rtppkt.h"

int nal_next(unsigned char *buf, unsigned char **nalptr, size_t *p_len, size_t max_len)
{
    int i;
    int start = -1;

    /* A pack shorter than a start code plus one payload byte holds no NAL. */
    if (max_len < 5)
        return -1;

    /* i <= max_len - 4 so a start code flush against the buffer end is still
       detected (buf[i+3] stays in bounds); a tighter bound would fold a short
       trailing NAL's start code into the previous NAL. */
    for (i = (*nalptr) - buf + *p_len; i <= (int)max_len - 4; i++) {
        if (buf[i] == 0x00 &&
                buf[i + 1] == 0x00 &&
                buf[i + 2] == 0x00 &&
                buf[i + 3] == 0x01) {
            if (start == -1) {
                i += 4;
                start = i;
            } else {
                *nalptr = &(buf[start]);
                while (buf[i - 1] == 0) i--;
                *p_len = i - start;
                return 0;
            }
        }
    }

    if (start == -1)
        return -1;

    *nalptr = &(buf[start]);
    *p_len = max_len - start;

    return 0;
}

int rtp_packetize_nal(const unsigned char *nal, int nal_len, int is_h265,
                      int max_payload, int marker_au_end,
                      rtp_emit_fn emit, void *ctx)
{
    /* A NAL shorter than its own header carries no payload — skip it. */
    if (nal_len < 4)
        return 0;

    int head = is_h265 ? 3 : 2;            /* FU indicator + FU header bytes */
    int nal_hdr = is_h265 ? 2 : 1;         /* original NAL header, not re-sent in FUs */
    unsigned int type = is_h265 ? ((nal[0] >> 1) & 0x3F) : (nal[0] & 0x1F);

    if (nal_len <= max_payload)
        return emit(ctx, NULL, 0, nal, nal_len, marker_au_end);

    /* Fragmentation unit. The FU indicator preserves F + NRI (H.264) or
       F + LayerId MSB (H.265) from the original header and substitutes the FU
       type; the FU header carries S/E flags and the original NAL type. */
    unsigned char fu[3];
    if (is_h265) {
        fu[0] = (49 << 1) | (nal[0] & 0x81);
        fu[1] = nal[1];
    } else {
        fu[0] = 28 | (nal[0] & 0xE0);
    }

    const unsigned char *p = nal + nal_hdr;
    int left = nal_len - nal_hdr;
    int chunk = max_payload - head;
    int first = 1;

    while (left > 0) {
        int body = left > chunk ? chunk : left;
        int last = (body == left);
        fu[head - 1] = (first ? 0x80 : 0) | (last ? 0x40 : 0) | type;
        int r = emit(ctx, fu, head, p, body, last ? marker_au_end : 0);
        if (r) return r;
        p += body;
        left -= body;
        first = 0;
    }

    return 0;
}

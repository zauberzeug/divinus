/* __split_nal: Annex-B NAL iterator used to harvest SDP parameter sets and
   packetize over RTP. The trailing-NAL length must be exact (no 2-byte OOB
   read past the pack) and a too-short pack must fail, not underflow the loop
   bound. Host-tested under ASan. */

#include <assert.h>
#include <stdio.h>

#include "rtsp/common.h"
#include "rtsp/list.h"
#include "rtsp/hash.h"
#include "rtsp/rtp.h"

int main(void) {
    /* One NAL, no trailing start code: length must be (max_len - start), i.e.
       the 6 payload bytes — not max_len + 2 - start (8, reading 2 past end). */
    unsigned char one[] = {0,0,0,1, 0x40,0x01,0x0c,0x01,0xff,0xff};
    unsigned char *nalp = one;
    size_t plen = 0;
    assert(__split_nal(one, &nalp, &plen, sizeof(one)) == SUCCESS);
    assert(nalp == one + 4);
    assert(plen == sizeof(one) - 4);

    /* Too-short pack must fail cleanly, not underflow `max_len - 5` into a
       huge size_t and walk off the buffer (ASan catches the old behavior). */
    unsigned char tiny[] = {0,0,1};
    nalp = tiny; plen = 0;
    assert(__split_nal(tiny, &nalp, &plen, sizeof(tiny)) == FAILURE);

    /* Two NALs: the first (followed by a start code) takes the i-start path,
       the second (trailing) takes the fixed max_len-start path. */
    unsigned char two[] = {0,0,0,1, 0x44,0x44, 0,0,0,1, 0x55,0x55,0x55};
    nalp = two; plen = 0;
    assert(__split_nal(two, &nalp, &plen, sizeof(two)) == SUCCESS);
    assert(nalp == two + 4 && plen == 2);            /* first NAL: 2 bytes */
    assert(__split_nal(two, &nalp, &plen, sizeof(two)) == SUCCESS);
    assert(nalp == two + 10 && plen == 3);           /* trailing NAL: 3 bytes */

    /* A short trailing NAL whose start code sits in the last bytes of the pack
       must still split out, not get folded into the previous NAL (the loop
       bound has to reach a start code flush against the buffer end). */
    unsigned char tail[] = {0,0,0,1, 0x44,0x44, 0,0,0,1, 0x55};
    nalp = tail; plen = 0;
    assert(__split_nal(tail, &nalp, &plen, sizeof(tail)) == SUCCESS);
    assert(nalp == tail + 4 && plen == 2);           /* first NAL stays 2 bytes */
    assert(__split_nal(tail, &nalp, &plen, sizeof(tail)) == SUCCESS);
    assert(nalp == tail + 10 && plen == 1);          /* 1-byte trailing NAL split out */

    puts("test_rtp_split: OK");
    return 0;
}

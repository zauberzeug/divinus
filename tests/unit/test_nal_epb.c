/* nal_decode (moov.c) emulation-prevention-byte stripper: correctness, and the
   short-input case where `i_size - 3` underflowed a size_t bound and read past
   the source buffer. Inputs are heap-allocated at their exact size so ASan's
   redzones trip on the old out-of-bounds read. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fmt/moov.h"

/* Run nal_decode against a src buffer sized to exactly i_size, so an over-read
   lands in an ASan redzone instead of slack. */
static size_t decode_exact(const uint8_t *src, size_t i_size, uint8_t *dst) {
    uint8_t *tight = malloc(i_size ? i_size : 1);
    memcpy(tight, src, i_size);
    size_t out = nal_decode(tight, dst, i_size);
    free(tight);
    return out;
}

static void eq(const uint8_t *src, size_t i_size,
               const uint8_t *want, size_t want_len) {
    uint8_t dst[64] = {0};
    size_t out = decode_exact(src, i_size, dst);
    if (out != want_len || memcmp(dst, want, want_len)) {
        fprintf(stderr, "nal_decode(i_size=%zu) -> len %zu, want %zu\n",
                i_size, out, want_len);
        assert(0);
    }
}

int main(void) {
    /* Short inputs: today (i_size - 3) underflows to ~SIZE_MAX, so the bound is
       "always true" and p_src[i+1]/[i+2] read past the buffer (ASan abort).
       After the fix the triplet check is skipped and bytes copy verbatim. */
    eq((const uint8_t[]){0x00}, 1, (const uint8_t[]){0x00}, 1);
    eq((const uint8_t[]){0x00, 0x00}, 2, (const uint8_t[]){0x00, 0x00}, 2);
    eq((const uint8_t[]){0x00, 0x00, 0x03}, 3,
       (const uint8_t[]){0x00, 0x00, 0x03}, 3);
    eq((const uint8_t[]){}, 0, (const uint8_t[]){}, 0);

    /* Emulation-prevention removal still works: 00 00 03 -> 00 00. */
    eq((const uint8_t[]){0x00, 0x00, 0x03, 0x04, 0x05}, 5,
       (const uint8_t[]){0x00, 0x00, 0x04, 0x05}, 4);
    /* Two EPB sequences. */
    eq((const uint8_t[]){0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x02}, 8,
       (const uint8_t[]){0x00, 0x00, 0x01, 0x00, 0x00, 0x02}, 6);
    /* No EPB: passthrough. */
    eq((const uint8_t[]){0x41, 0x9A, 0x00, 0x01, 0x02}, 5,
       (const uint8_t[]){0x41, 0x9A, 0x00, 0x01, 0x02}, 5);
    /* A trailing 00 00 with no 03 is untouched. */
    eq((const uint8_t[]){0x12, 0x00, 0x00}, 3,
       (const uint8_t[]){0x12, 0x00, 0x00}, 3);

    puts("test_nal_epb: OK");
    return 0;
}

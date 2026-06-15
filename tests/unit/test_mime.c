/* mime base64/base16 encoders: correctness across every length residue,
   including the short inputs that overflowed the output buffer (size_t
   (len-3) underflow + undersized calloc). Host-tested under ASan. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rtsp/common.h"
#include "rtsp/mime.h"

static void eq64(const char *src, size_t len, const char *want) {
    mime_encoded_handle h = mime_base64_create((char *)src, len);
    assert(h && h->result);
    if (strcmp(h->result, want)) {
        fprintf(stderr, "base64(len=%zu) = \"%s\", want \"%s\"\n",
                len, h->result, want);
        assert(0);
    }
    mime_encoded_delete(h);
}

int main(void) {
    /* One byte: today (len-3) underflows to ~SIZE_MAX and calloc(len*2+1=3)
       is too small for "Zw==" (5 bytes) — heap overflow under ASan. */
    eq64("\x67", 1, "Zw==");
    /* Every length residue (1/2/0), incl. the two-byte short case that was
       also undersized, and multi-iteration of the main loop. */
    eq64("M", 1, "TQ==");
    eq64("Ma", 2, "TWE=");
    eq64("Man", 3, "TWFu");
    eq64("abcd", 4, "YWJjZA==");
    eq64("abcde", 5, "YWJjZGU=");
    eq64("abcdef", 6, "YWJjZGVm");
    eq64("any carnal pleasure.", 20, "YW55IGNhcm5hbCBwbGVhc3VyZS4=");
    puts("test_mime: OK");
    return 0;
}

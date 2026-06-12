/* Smoke test: proves the host test toolchain compiles and links real divinus
   source (src/fmt/nal.c) and that asserts/sanitizers fire. Real coverage of
   fmt/ and rtsp/ is added by the unit-test cards; this only exercises the
   plumbing plus a couple of NAL header / start-code basics. */

#include <assert.h>
#include <stdio.h>

#include "fmt/nal.h"

static void test_start_codes(void) {
    const char sc4[] = {0, 0, 0, 1, 0x65};
    const char sc3[] = {0, 0, 1, 0x67, 0};
    const char none[] = {1, 2, 3, 4};

    /* nal_chk4 accepts BOTH the 3-byte (00 00 01) and 4-byte (00 00 00 01)
       Annex-B start codes; nal_chk3 accepts only the 3-byte form. */
    assert(nal_chk4(sc4, 0));
    assert(nal_chk3(sc3, 0));
    assert(nal_chk4(sc3, 0));
    assert(!nal_chk3(sc4, 0));
    assert(!nal_chk4(none, 0));
}

static void test_parse_header(void) {
    struct NAL h264 = {.isH265 = 0};
    nal_parse_header(&h264, 0x65); /* H.264 IDR slice */
    assert(h264.unit_type == NalUnitType_CodedSliceIdr);
    assert(h264.ref_idc == 3);
    assert(!h264.forbidden_zero_bit);

    struct NAL h265 = {.isH265 = 1};
    nal_parse_header(&h265, 0x40); /* HEVC VPS */
    assert(h265.unit_type == NalUnitType_VPS_HEVC);
}

int main(void) {
    test_start_codes();
    test_parse_header();
    puts("test_nal: OK");
    return 0;
}

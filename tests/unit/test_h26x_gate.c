#include <assert.h>
#include <stdio.h>

#include "h26x_gate.h"

static void test_h264_access_unit_resumes_at_sps(void) {
    hal_vidnalu nalus[] = {
        {.type = NalUnitType_SEI},
        {.type = NalUnitType_SPS},
        {.type = NalUnitType_PPS},
        {.type = NalUnitType_CodedSliceIdr},
    };

    assert(h26x_resume_index(nalus, 4) == 1);
}

static void test_hevc_glued_keyframe_resumes_at_vps(void) {
    hal_vidnalu nalus[] = {
        {.type = NalUnitType_VPS_HEVC},
        {.type = NalUnitType_SPS_HEVC},
        {.type = NalUnitType_PPS_HEVC},
        {.type = NalUnitType_CodedSliceIdr},
    };

    assert(h26x_resume_index(nalus, 4) == 0);
}

static void test_hevc_without_vps_resumes_at_sps(void) {
    hal_vidnalu nalus[] = {
        {.type = NalUnitType_SEI_HEVC},
        {.type = NalUnitType_SPS_HEVC},
        {.type = NalUnitType_PPS_HEVC},
        {.type = NalUnitType_CodedSliceIdr},
    };

    assert(h26x_resume_index(nalus, 4) == 1);
}

static void test_p_frame_access_unit_has_no_resume_point(void) {
    hal_vidnalu nalus[] = {
        {.type = NalUnitType_CodedSliceNonIdr},
    };

    assert(h26x_resume_index(nalus, 1) == -1);
}

static void test_empty_or_zero_count_has_no_resume_point(void) {
    hal_vidnalu nalus[] = {
        {.type = NalUnitType_SPS},
    };

    assert(h26x_resume_index(nalus, 0) == -1);
    assert(h26x_resume_index(NULL, 0) == -1);
}

int main(void) {
    test_h264_access_unit_resumes_at_sps();
    test_hevc_glued_keyframe_resumes_at_vps();
    test_hevc_without_vps_resumes_at_sps();
    test_p_frame_access_unit_has_no_resume_point();
    test_empty_or_zero_count_has_no_resume_point();
    puts("test_h26x_gate: OK");
    return 0;
}

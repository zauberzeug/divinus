#pragma once

#include "fmt/nal.h"
#include "hal/types.h"

static inline int h26x_resume_index(const hal_vidnalu *nalus, int count) {
    if (!nalus || count <= 0)
        return -1;

    for (int i = 0; i < count; i++) {
        if (nalus[i].type == NalUnitType_VPS_HEVC ||
            nalus[i].type == NalUnitType_SPS ||
            nalus[i].type == NalUnitType_SPS_HEVC)
            return i;
    }

    return -1;
}

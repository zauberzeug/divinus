#include "gain.h"

static int gain_value_invalid(unsigned int value) {
    return value && value < GAIN_1X;
}

int gain_limits_merge(hal_gainlimits *cur, const hal_gainlimits *req) {
    if (gain_value_invalid(req->minSensorGain) ||
        gain_value_invalid(req->maxSensorGain) ||
        gain_value_invalid(req->minIspGain) ||
        gain_value_invalid(req->maxIspGain))
        return -1;

    hal_gainlimits merged = *cur;
    if (req->minSensorGain) merged.minSensorGain = req->minSensorGain;
    if (req->maxSensorGain) merged.maxSensorGain = req->maxSensorGain;
    if (req->minIspGain) merged.minIspGain = req->minIspGain;
    if (req->maxIspGain) merged.maxIspGain = req->maxIspGain;

    if (merged.minSensorGain > merged.maxSensorGain ||
        merged.minIspGain > merged.maxIspGain)
        return -1;

    *cur = merged;
    return 0;
}

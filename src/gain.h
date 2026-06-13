#pragma once

#include "hal/types.h"

/* SigmaStar AE gain convention: 1024 == 1x */
#define GAIN_1X 1024

int gain_limits_merge(hal_gainlimits *cur, const hal_gainlimits *req);

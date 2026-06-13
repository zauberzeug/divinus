/* Vendor-free AE gain-limit merge rules: SigmaStar convention 1024 == 1x,
   zero fields mean "leave unchanged". */

#include <assert.h>
#include <stdio.h>

#include "gain.h"

static const hal_gainlimits base = {
    .minSensorGain = 1024, .maxSensorGain = 65536,
    .minIspGain = 1024, .maxIspGain = 4096,
};

static void test_unset_request_keeps_current(void) {
    hal_gainlimits cur = base;
    hal_gainlimits req = {0};

    assert(gain_limits_merge(&cur, &req) == 0);
    assert(cur.minSensorGain == base.minSensorGain);
    assert(cur.maxSensorGain == base.maxSensorGain);
    assert(cur.minIspGain == base.minIspGain);
    assert(cur.maxIspGain == base.maxIspGain);
}

static void test_set_fields_override(void) {
    hal_gainlimits cur = base;
    hal_gainlimits req = {.maxSensorGain = 2048, .maxIspGain = 1024};

    assert(gain_limits_merge(&cur, &req) == 0);
    assert(cur.minSensorGain == base.minSensorGain);
    assert(cur.maxSensorGain == 2048);
    assert(cur.minIspGain == base.minIspGain);
    assert(cur.maxIspGain == 1024);
}

static void test_below_one_x_rejected(void) {
    /* 1024 == 1x is the floor; smaller nonzero values are unit mistakes */
    hal_gainlimits cur = base;
    hal_gainlimits req = {.maxSensorGain = 16};

    assert(gain_limits_merge(&cur, &req) != 0);
    assert(cur.maxSensorGain == base.maxSensorGain);

    req = (hal_gainlimits){.minIspGain = 1023};
    assert(gain_limits_merge(&cur, &req) != 0);
    assert(cur.minIspGain == base.minIspGain);
}

static void test_min_above_max_rejected(void) {
    hal_gainlimits cur = base;
    hal_gainlimits req = {.minSensorGain = 8192, .maxSensorGain = 4096};

    assert(gain_limits_merge(&cur, &req) != 0);
    assert(cur.minSensorGain == base.minSensorGain);
    assert(cur.maxSensorGain == base.maxSensorGain);

    /* also when one end comes from the current limits */
    req = (hal_gainlimits){.minIspGain = 8192};
    assert(gain_limits_merge(&cur, &req) != 0);
    assert(cur.minIspGain == base.minIspGain);
}

int main(void) {
    test_unset_request_keeps_current();
    test_set_fields_override();
    test_below_one_x_rejected();
    test_min_above_max_rejected();
    puts("test_gain: OK");
    return 0;
}

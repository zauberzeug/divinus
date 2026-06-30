/* Behavioral tests for parse_api_int(), the range-checked integer parser the
   /api/* query setters use. It parses a decimal value, clamps it into
   [min, max], and writes *out; a non-numeric value is rejected and leaves
   *out untouched so the caller keeps its prior value. */

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "hal/tools.h"

/* A value within range is written through unchanged, with no width loss. */
static void test_accepts_in_range_value(void) {
    int out = -1;
    assert(parse_api_int("50000", 32, INT_MAX, &out));
    assert(out == 50000);

    out = -1;
    assert(parse_api_int("2097152", 0, INT_MAX, &out));
    assert(out == 2097152);
}

/* A value above max is clamped down to max. */
static void test_clamps_above_max(void) {
    int out = 0;
    assert(parse_api_int("32768", 0, SHRT_MAX, &out));
    assert(out == SHRT_MAX);

    out = 0;
    assert(parse_api_int("200", 0, 95, &out));
    assert(out == 95);
}

/* A value below min is clamped up to min. */
static void test_clamps_below_min(void) {
    int out = 0;
    assert(parse_api_int("16", 32, INT_MAX, &out));
    assert(out == 32);

    out = 0;
    assert(parse_api_int("-61", -60, 30, &out));
    assert(out == -60);
}

/* Exact boundaries pass through untouched. */
static void test_boundaries(void) {
    int out = 0;
    assert(parse_api_int("32767", 0, SHRT_MAX, &out));
    assert(out == 32767);

    out = 0;
    assert(parse_api_int("-60", -60, 30, &out));
    assert(out == -60);
}

/* Non-numeric input is rejected and the destination keeps its prior value. */
static void test_rejects_non_numeric(void) {
    int out = 1024;
    assert(!parse_api_int("abc", 0, INT_MAX, &out));
    assert(out == 1024);
    assert(!parse_api_int("", 0, INT_MAX, &out));
    assert(out == 1024);
}

int main(void) {
    test_accepts_in_range_value();
    test_clamps_above_max();
    test_clamps_below_min();
    test_boundaries();
    test_rejects_non_numeric();
    puts("test_api_int: OK");
    return 0;
}

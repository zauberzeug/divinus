/* Behavioral tests for parse_api_int(), the range-checked integer parser the
   /api/* query setters use. The setters previously parsed every numeric param
   through a `short result = strtol(...)`, so any value >32767 truncated before
   the widening assignment to the (32-bit) config field: setting mp4 bitrate to
   50000 stored -15536, which then failed config reload and crash-looped the
   streamer. This mirrors parse_int() on the config-file path. */

#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "hal/tools.h"

/* The case that bricked the streamer: a bitrate of 50000 must round-trip as
   50000, never the `short`-truncated -15536. */
static void test_accepts_value_above_short_range(void) {
    int out = -1;
    assert(parse_api_int("50000", 32, INT_MAX, &out));
    assert(out == 50000);
}

/* The common 2 MiB record segment size used to wrap through a short. */
static void test_accepts_large_record_segment_size(void) {
    int out = -1;
    assert(parse_api_int("2097152", 0, INT_MAX, &out));
    assert(out == 2097152);
}

/* Exact boundary of the old short ceiling against a short-ranged field. */
static void test_short_field_boundary(void) {
    int out = 7;
    assert(parse_api_int("32767", 0, SHRT_MAX, &out));
    assert(out == 32767);

    out = 7;
    assert(!parse_api_int("32768", 0, SHRT_MAX, &out));
    assert(out == 7); /* over range: caller keeps its prior value */
}

/* Out-of-range and garbage leave the destination untouched so the handler
   retains its previously configured value, matching the old `remain != value`
   keep-prior contract (now also bounded). */
static void test_rejects_and_preserves_prior(void) {
    int out = 1024;
    assert(!parse_api_int("16", 32, INT_MAX, &out));   /* below min */
    assert(out == 1024);
    assert(!parse_api_int("abc", 0, INT_MAX, &out));   /* not a number */
    assert(out == 1024);
    assert(!parse_api_int("", 0, INT_MAX, &out));      /* empty */
    assert(out == 1024);
}

/* Signed ranges (e.g. audio gain [-60, 30]) parse negatives and bound them. */
static void test_signed_range(void) {
    int out = 0;
    assert(parse_api_int("-60", -60, 30, &out));
    assert(out == -60);
    out = 0;
    assert(!parse_api_int("-61", -60, 30, &out));
    assert(out == 0);
}

int main(void) {
    test_accepts_value_above_short_range();
    test_accepts_large_record_segment_size();
    test_short_field_boundary();
    test_rejects_and_preserves_prior();
    test_signed_range();
    puts("test_api_int: OK");
    return 0;
}

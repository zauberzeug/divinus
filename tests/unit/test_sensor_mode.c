/* Sensor mode table: parse the SigmaStar /proc dump into the normalised
   sensor_mode list that divinus logs at startup, and render it. The vendor
   fnGetResolution call can't enumerate the table without corrupting the
   pipeline, so /proc is the source of truth. */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hal/sensor_mode.h"

/* A trimmed SigmaStar mi_sensor0 dump: the resolution table surrounded by the
   header, the Cur line, and a plane-info row the parser must all ignore. */
static const char PROC_SAMPLE[] =
    "       PadId  PlaneMode  bEnable  bmirror  bflip  fps  ResCnt\n"
    "      0          0        1        0      0    5       4      MIPI        2         2\n"
    "       Res        strResDesc  CropX  CropY  CropW  CropH  OutW  OutH  MaxFps  MinFps\n"
    "             2560x1920@30fps      0      0   2560   1920  2560  1920      30       3\n"
    "             2560x1920@60fps      0      0   2560   1920  2560  1920      60       3\n"
    "             2400x1350@90fps      0      0   2560   1440  2400  1350      90       3\n"
    "            1920x1080@120fps      0      0   1920   1080  1920  1080     120       3\n"
    "       Cur   2560x1920@30fps      0      0   2560   1920  2560  1920      30       3\n"
    "      0        0         IMX335_MIPI       RG     10BPP        0      0      0   2560   1920\n";

static void test_parse_proc_extracts_only_resolution_rows(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = sensor_mode_parse_proc(PROC_SAMPLE, m, SENSOR_MODE_MAX);
    assert(n == 4);  /* header, Cur, and plane-info rows are skipped */
    assert(strcmp(m[0].desc, "2560x1920@30fps") == 0);
    assert(m[0].crop_width == 2560 && m[0].crop_height == 1920);
    assert(m[0].out_width == 2560 && m[0].out_height == 1920);
    assert(m[0].max_fps == 30 && m[0].min_fps == 3);
    /* index 2 has a crop wider than its output (2560x1440 -> 2400x1350) */
    assert(m[2].crop_width == 2560 && m[2].crop_height == 1440);
    assert(m[2].out_width == 2400 && m[2].out_height == 1350 && m[2].max_fps == 90);
    assert(m[3].crop_width == 1920 && m[3].crop_height == 1080 && m[3].max_fps == 120);
}

static void test_parse_proc_caps_at_max(void) {
    sensor_mode m[2];
    assert(sensor_mode_parse_proc(PROC_SAMPLE, m, 2) == 2);
}

static void test_parse_proc_empty_on_garbage(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    assert(sensor_mode_parse_proc("no table here\njust noise\n", m,
                                  SENSOR_MODE_MAX) == 0);
}

/* Load the four-mode table from the sample dump for the selector tests. */
static int load_modes(sensor_mode *m) {
    int n = sensor_mode_parse_proc(PROC_SAMPLE, m, SENSOR_MODE_MAX);
    assert(n == 4);
    return n;
}

/* A negative profile keeps the legacy first-fit pick: the first mode whose
   crop covers the request at >= the requested fps. */
static void test_select_auto_first_fit(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = load_modes(m);
    /* 2560x1920@60 needs crop >= 2560x1920 and >= 60fps -> index 1. */
    sensor_mode_choice c = sensor_mode_select(-1, m, n, 2560, 1920, 60);
    assert(c.index == 1 && c.fps == 60);
    /* 1920x1080@120: modes 0-2 are <120fps or don't crop to it -> index 3. */
    c = sensor_mode_select(-1, m, n, 1920, 1080, 120);
    assert(c.index == 3 && c.fps == 120);
    /* Default low-res 30fps request still lands on index 0 (no behaviour change). */
    c = sensor_mode_select(-1, m, n, 1440, 1080, 30);
    assert(c.index == 0 && c.fps == 30);
}

/* Auto mode returns -1 when no mode satisfies the request (today's failure). */
static void test_select_auto_no_fit(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = load_modes(m);
    sensor_mode_choice c = sensor_mode_select(-1, m, n, 2560, 1920, 200);
    assert(c.index < 0);
}

/* A profile in range forces that mode regardless of crop/output fit. */
static void test_select_forced_in_range(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = load_modes(m);
    /* Force index 3 even though the request would first-fit to 0. */
    sensor_mode_choice c = sensor_mode_select(3, m, n, 1440, 1080, 60);
    assert(c.index == 3 && c.fps == 60);
}

/* Forcing a mode caps the fps to that mode's ceiling instead of crashing the
   pipeline (fnSetFramerate over the ceiling returns 0xa01b201f). */
static void test_select_forced_caps_fps(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = load_modes(m);
    /* Mode 0 maxes at 30fps; a 60fps request is clamped down. */
    sensor_mode_choice c = sensor_mode_select(0, m, n, 1440, 1080, 60);
    assert(c.index == 0 && c.fps == 30);
    /* Under the ceiling is left untouched. */
    c = sensor_mode_select(2, m, n, 1440, 1080, 45);
    assert(c.index == 2 && c.fps == 45);
}

/* An out-of-range profile falls back to first-fit rather than indexing past
   the table (a hand-edited config can't crash the picker). */
static void test_select_forced_out_of_range_falls_back(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    int n = load_modes(m);
    sensor_mode_choice c = sensor_mode_select(9, m, n, 1440, 1080, 30);
    assert(c.index == 0 && c.fps == 30);
}

/* An empty table yields no choice. */
static void test_select_empty_table(void) {
    sensor_mode m[SENSOR_MODE_MAX];
    sensor_mode_choice c = sensor_mode_select(-1, m, 0, 1440, 1080, 30);
    assert(c.index < 0);
    c = sensor_mode_select(0, m, 0, 1440, 1080, 30);
    assert(c.index < 0);
}

/* Fake vendor query backed by the parsed sample table, cycling by modulo so
   the clamp test can walk past the four real modes. Records the highest index
   queried to assert the ascending-and-stop enumeration sensor_mode_pick must
   honor (querying a mode it does not then apply corrupts the real pipeline). */
static sensor_mode fake_table[SENSOR_MODE_MAX];
static int fake_count;
static int fake_max_queried;
static int fake_fail_at;   /* index that returns an error; -1 = never */

static void fake_reset(void) {
    fake_count = load_modes(fake_table);
    fake_max_queried = -1;
    fake_fail_at = -1;
}

static int fake_query(int index, sensor_mode *out) {
    fake_max_queried = index;
    if (index == fake_fail_at)
        return 7;  /* distinctive vendor-style error code */
    *out = fake_table[index % fake_count];
    return 0;
}

/* Auto pick stops at the first fitting mode and never queries past it. */
static void test_pick_auto_stops_at_first_fit(void) {
    fake_reset();
    sensor_mode_choice c;
    int ret = sensor_mode_pick("t", fake_query, fake_count, -1, 1440, 1080, 30, &c);
    assert(ret == 0);
    assert(c.index == 0 && c.fps == 30);
    assert(fake_max_queried == 0);
}

/* Auto pick walks ascending until a mode satisfies the request. */
static void test_pick_auto_walks_to_higher_mode(void) {
    fake_reset();
    sensor_mode_choice c;
    int ret = sensor_mode_pick("t", fake_query, fake_count, -1, 1920, 1080, 120, &c);
    assert(ret == 0);
    assert(c.index == 3 && c.fps == 120);
    assert(fake_max_queried == 3);
}

/* A forced profile queries only up to that index and caps fps to its ceiling. */
static void test_pick_forced_queries_to_index_and_caps_fps(void) {
    fake_reset();
    sensor_mode_choice c;
    int ret = sensor_mode_pick("t", fake_query, fake_count, 3, 1920, 1080, 240, &c);
    assert(ret == 0);
    assert(c.index == 3 && c.fps == 120);  /* capped to mode 3's max */
    assert(fake_max_queried == 3);
}

/* A query error is returned verbatim and aborts enumeration at that index. */
static void test_pick_query_error_propagates(void) {
    fake_reset();
    fake_fail_at = 2;
    sensor_mode_choice c = { .index = 0, .fps = 0 };
    int ret = sensor_mode_pick("t", fake_query, fake_count, -1, 1920, 1080, 120, &c);
    assert(ret == 7);
    assert(fake_max_queried == 2);
}

/* count above SENSOR_MODE_MAX is clamped; a no-fit request walks the clamped
   range and fails without indexing out of bounds. */
static void test_pick_clamps_count(void) {
    fake_reset();
    sensor_mode_choice c;
    int ret = sensor_mode_pick("t", fake_query, 1000, -1, 9999, 9999, 30, &c);
    assert(ret == EXIT_FAILURE);
    assert(fake_max_queried == SENSOR_MODE_MAX - 1);
}

/* An out-of-range forced profile falls back to first-fit without querying the
   bogus index. */
static void test_pick_forced_out_of_range_first_fit(void) {
    fake_reset();
    sensor_mode_choice c;
    int ret = sensor_mode_pick("t", fake_query, fake_count, 10, 1440, 1080, 30, &c);
    assert(ret == 0);
    assert(c.index == 0);
    assert(fake_max_queried == 0);
}

int main(void) {
    test_parse_proc_extracts_only_resolution_rows();
    test_parse_proc_caps_at_max();
    test_parse_proc_empty_on_garbage();
    test_select_auto_first_fit();
    test_select_auto_no_fit();
    test_select_forced_in_range();
    test_select_forced_caps_fps();
    test_select_forced_out_of_range_falls_back();
    test_select_empty_table();
    test_pick_auto_stops_at_first_fit();
    test_pick_auto_walks_to_higher_mode();
    test_pick_forced_queries_to_index_and_caps_fps();
    test_pick_query_error_propagates();
    test_pick_clamps_count();
    test_pick_forced_out_of_range_first_fit();

    /* Smoke the logger on the parsed table under ASan/UBSan (catches desc
       overread / bad format). */
    sensor_mode m[SENSOR_MODE_MAX];
    int n = sensor_mode_parse_proc(PROC_SAMPLE, m, SENSOR_MODE_MAX);
    sensor_mode_log("test_snr", m, n);

    puts("test_sensor_mode: OK");
    return 0;
}

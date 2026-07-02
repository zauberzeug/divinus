/* Sensor mode table: parse the SigmaStar /proc dump into the normalised
   sensor_mode list that divinus logs at startup, and render it. The vendor
   fnGetResolution call can't enumerate the table without corrupting the
   pipeline, so /proc is the source of truth. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

int main(void) {
    test_parse_proc_extracts_only_resolution_rows();
    test_parse_proc_caps_at_max();
    test_parse_proc_empty_on_garbage();

    /* Smoke the logger on the parsed table under ASan/UBSan (catches desc
       overread / bad format). */
    sensor_mode m[SENSOR_MODE_MAX];
    int n = sensor_mode_parse_proc(PROC_SAMPLE, m, SENSOR_MODE_MAX);
    sensor_mode_log("test_snr", m, n);

    puts("test_sensor_mode: OK");
    return 0;
}

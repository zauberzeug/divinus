/* Frame-rate control policy: PTS pacing that holds a stream at its
   configured rate, the effective sensor rate for a shutter time, and the
   shutter window that lets the sensor rate rise again. */

#include <assert.h>
#include <stdio.h>

#include "hal/frc.h"

static void test_pacer_decimates_to_configured_rate(void) {
    /* mjpeg configured at 5 fps while the sensor delivers 15 (66.7 ms PTS
       steps): exactly every third frame is forwarded. */
    frc_pacer pacer = {0};
    int sent = 0;

    for (int i = 0; i < 45; i++)
        if (frc_pace_due(&pacer, 5, i * 66667ull))
            sent++;
    assert(sent == 15);
}

static void test_pacer_passes_at_rate_source_through(void) {
    /* A source already at the configured rate loses nothing. */
    frc_pacer pacer = {0};

    for (int i = 0; i < 30; i++)
        assert(frc_pace_due(&pacer, 5, i * 200000ull));
}

static void test_pacer_follows_slow_sensor(void) {
    /* Long exposure dropped the sensor to 2 fps: a 5 fps stream follows the
       sensor instead of being decimated further. */
    frc_pacer pacer = {0};

    for (int i = 0; i < 10; i++)
        assert(frc_pace_due(&pacer, 5, i * 500000ull));
}

static void test_pacer_recovers_without_burst(void) {
    /* After a slow phase the pacer must not "catch up" by passing a burst
       of frames once the sensor returns to full rate. */
    frc_pacer pacer = {0};
    unsigned long long pts = 0;
    int sent = 0;

    for (int i = 0; i < 5; i++, pts += 500000)
        frc_pace_due(&pacer, 5, pts);
    for (int i = 0; i < 45; i++, pts += 66667)
        if (frc_pace_due(&pacer, 5, pts))
            sent++;
    assert(sent >= 15 && sent <= 16);
}

static void test_pacer_disabled_without_rate(void) {
    frc_pacer pacer = {0};

    assert(frc_pace_due(&pacer, 0, 123456));
}

static void test_effective_fps_follows_shutter(void) {
    /* Auto exposure (0) and any shutter that fits in the configured frame
       time keep the configured rate. */
    assert(frc_effective_fps(15, 0) == 15);
    assert(frc_effective_fps(15, 33333) == 15);
    assert(frc_effective_fps(15, 66666) == 15);

    /* A shutter longer than the frame time forces the sensor down to the
       rate whose frame time still fits it. */
    assert(frc_effective_fps(15, 100000) == 10);
    assert(frc_effective_fps(15, 333333) == 3);
    assert(frc_effective_fps(30, 40000) == 25);

    /* The sensor cannot go below 3 fps; the ISP caps the shutter instead. */
    assert(frc_effective_fps(15, 1000000) == 3);

    /* A long shutter must never raise the rate above the configured one:
       a 40 ms shutter fits in a 15 fps frame time, 25 fps is not "needed". */
    assert(frc_effective_fps(15, 40000) == 15);

    /* No configured rate yet: nothing to derive from. */
    assert(frc_effective_fps(0, 333333) == 0);
}

static void test_shutter_cap_for_raising_the_rate(void) {
    /* The sensor silently refuses a higher rate while the applied shutter
       exceeds the new frame period, so the shutter window used during the
       transition must fit that period. Auto exposure (0) gets capped at the
       frame period... */
    assert(frc_shutter_cap(15, 0) == 66666);

    /* ...a fixed shutter that already fits is used as-is... */
    assert(frc_shutter_cap(15, 10000) == 10000);
    assert(frc_shutter_cap(14, 70000) == 70000);

    /* ...and at the 3 fps floor the full auto range fits the frame period.
       Without a rate (0) the cap is just the auto-exposure ceiling. */
    assert(frc_shutter_cap(3, 0) == 333333);
    assert(frc_shutter_cap(0, 0) == 333333);
}

int main(void) {
    test_pacer_decimates_to_configured_rate();
    test_pacer_passes_at_rate_source_through();
    test_pacer_follows_slow_sensor();
    test_pacer_recovers_without_burst();
    test_pacer_disabled_without_rate();
    test_effective_fps_follows_shutter();
    test_shutter_cap_for_raising_the_rate();
    puts("test_frc: OK");
    return 0;
}

/* Frame-rate control policy: PTS pacing that holds a stream at its
   configured rate, and the shutter window clamped to the frame budget
   (the frame rate is the contract; the shutter never exceeds it). */

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
    /* A source already at or below its configured rate (here a 2 fps sensor
       feeding a 5 fps stream) is passed through, never decimated further. */
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

static void test_shutter_cap_clamps_to_frame_budget(void) {
    /* The frame budget (the longest exposure that holds the rate) is the
       cap with no shutter requested (0) — this is what auto and `max` use. */
    assert(frc_shutter_cap(15, 0) == 66666);

    /* A fixed shutter that already fits the frame time is used as-is... */
    assert(frc_shutter_cap(15, 10000) == 10000);
    assert(frc_shutter_cap(14, 70000) == 70000);

    /* ...and one longer than the frame time is clamped down to the budget
       (the rate is never traded for shutter time). */
    assert(frc_shutter_cap(15, 100000) == 66666);
    assert(frc_shutter_cap(30, 40000) == 33333);

    /* At/under the 3 fps floor the full auto range fits the frame period;
       without a rate (0) the cap is just the auto-exposure ceiling. */
    assert(frc_shutter_cap(3, 0) == 333333);
    assert(frc_shutter_cap(0, 0) == 333333);
}

int main(void) {
    test_pacer_decimates_to_configured_rate();
    test_pacer_passes_at_rate_source_through();
    test_pacer_follows_slow_sensor();
    test_pacer_recovers_without_burst();
    test_pacer_disabled_without_rate();
    test_shutter_cap_clamps_to_frame_budget();
    puts("test_frc: OK");
    return 0;
}

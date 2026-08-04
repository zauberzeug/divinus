#pragma once

#include <stdbool.h>

/* Sentinel for the configured exposure (app_config.exposure) and
   i6_sensor_exposure(): pin the shutter to the full frame time, i.e. the
   longest exposure that still holds the configured frame rate. */
#define EXPOSURE_MAX 0xFFFFFFFFu

typedef struct {
    unsigned long long nextDueUs;
} frc_pacer;

bool frc_pace_due(frc_pacer *pacer, int fps, unsigned long long ptsUs);
unsigned int frc_shutter_cap(int fps, unsigned int shutterUs);

/* The shutter that is safe across a rate change: no longer than either the old
   or the new frame period. The sensor library silently ignores a rate increase
   while the applied shutter still exceeds the shorter new period, so the
   shutter is narrowed to this before the rate is written. from_fps of 0 means
   "no rate applied yet" -- the bring-up case, where only the target period
   constrains the shutter. */
unsigned int frc_rate_change_shutter(int from_fps, int to_fps);

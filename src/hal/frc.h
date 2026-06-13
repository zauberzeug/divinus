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

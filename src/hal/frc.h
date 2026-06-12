#pragma once

#include <stdbool.h>

typedef struct {
    unsigned long long nextDueUs;
} frc_pacer;

bool frc_pace_due(frc_pacer *pacer, int fps, unsigned long long ptsUs);
int frc_effective_fps(int configFps, unsigned int shutterUs);
unsigned int frc_shutter_cap(int fps, unsigned int shutterUs);

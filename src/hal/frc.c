#include "frc.h"

bool frc_pace_due(frc_pacer *pacer, int fps, unsigned long long ptsUs)
{
    if (fps <= 0)
        return true;

    unsigned long long period = 1000000ull / fps;

    if (ptsUs < pacer->nextDueUs)
        return false;
    pacer->nextDueUs += period;
    if (pacer->nextDueUs <= ptsUs)
        pacer->nextDueUs = ptsUs + period;
    return true;
}

int frc_effective_fps(int configFps, unsigned int shutterUs)
{
    if (configFps <= 0 || shutterUs == 0 || shutterUs <= 1000000u / configFps)
        return configFps;

    int fps = 1000000 / shutterUs;
    return fps < 3 ? 3 : fps;
}

unsigned int frc_shutter_cap(int fps, unsigned int shutterUs)
{
    unsigned int max = shutterUs ? shutterUs : 333333;

    if (fps > 0 && max > 1000000u / fps)
        max = 1000000u / fps;
    return max;
}

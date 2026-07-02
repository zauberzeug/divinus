#pragma once

/* Upper bound for the enumerated mode table (real sensors expose a handful of
   modes; this bounds the HAL stack array). */
#define SENSOR_MODE_MAX 32

/* One enumerated sensor resolution, normalised across the vendor HALs. */
typedef struct {
    char desc[32];
    unsigned short crop_width;
    unsigned short crop_height;
    unsigned short out_width;
    unsigned short out_height;
    unsigned int min_fps;
    unsigned int max_fps;
} sensor_mode;

/* Log the enumerated table (index, desc, crop, output, fps range) via
   HAL_INFO under the given module tag. */
void sensor_mode_log(const char *mod, const sensor_mode *modes, int count);

/* Parse a SigmaStar /proc/mi_modules/mi_sensor/mi_sensorN dump into modes[],
   keeping only the resolution rows (desc like "WxH@Ffps" followed by the crop/
   output/fps columns). The vendor fnGetResolution call can't enumerate the
   table — querying a mode it does not then apply corrupts the pipeline — so
   the modes are read from /proc instead. Returns the count parsed, capped at
   max. */
int sensor_mode_parse_proc(const char *text, sensor_mode *modes, int max);

/* Read and parse the sensor's /proc resolution table. Returns the mode count,
   or -1 if the proc node can't be opened. */
int sensor_mode_read(int sensor_index, sensor_mode *modes, int max);

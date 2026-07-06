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

/* Outcome of picking a sensor mode for a stream request. index is the chosen
   mode, or -1 if none fits (empty table, or auto first-fit found nothing).
   fps is the requested rate capped to the chosen mode's max_fps, so an
   over-fast request maxes out the mode instead of crashing pipeline creation
   (fnSetFramerate above the ceiling returns 0xa01b201f). */
typedef struct {
    int index;
    unsigned int fps;
} sensor_mode_choice;

/* Pick a sensor mode for a stream request.
   - profile in [0, count): force that mode directly (manual override), with
     fps capped to its ceiling regardless of crop/output fit. The sensor then
     emits the mode's geometry and the existing scaler adapts the streams, so
     resolution maxes out at the mode automatically.
   - profile < 0 or out of range: legacy first-fit — the first mode whose crop
     covers req_width x req_height at >= req_fps; index -1 if none. This keeps
     the default picker unchanged when no override is configured. */
sensor_mode_choice sensor_mode_select(int profile, const sensor_mode *modes,
    int count, unsigned short req_width, unsigned short req_height,
    unsigned int req_fps);

/* Query one enumerated sensor mode by ascending index into *out (zeroed
   before the call). Returns 0 on success or the vendor error code. */
typedef int (*sensor_mode_query)(int index, sensor_mode *out);

/* Enumerate the vendor mode table through query and pick a mode for the
   request (sensor_mode_select semantics: profile forces an index, otherwise
   first-fit). The table is queried ascending and stops at the index that is
   committed, because querying a mode that is not then applied corrupts the
   pipeline. Logs the pick under mod. Returns 0 with *choice filled, the
   query's error verbatim, or EXIT_FAILURE when no mode fits. */
int sensor_mode_pick(const char *mod, sensor_mode_query query,
    unsigned int count, int profile, unsigned short req_width,
    unsigned short req_height, unsigned int req_fps,
    sensor_mode_choice *choice);

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

/* Cache the enumerated table once it has been read safely (while streaming),
   so request paths like the web API can serve it without re-reading /proc
   (reading the node mid-bring-up disturbs the sensor). */
void sensor_mode_cache(const sensor_mode *modes, int count);

/* Point *modes at the cached table and return its count (0 if none cached). */
int sensor_mode_cached(const sensor_mode **modes);

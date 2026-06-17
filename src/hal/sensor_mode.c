#include "sensor_mode.h"

#include "macros.h"

int sensor_mode_parse_proc(const char *text, sensor_mode *modes, int max)
{
    int count = 0;
    for (const char *p = text; *p && count < max; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        char desc[32];
        int cropx, cropy, cropw, croph, outw, outh, maxfps, minfps;
        // A resolution row is the desc ("WxH@Ffps") plus the eight columns;
        // header/Cur/other-section lines fail the field count or the desc shape.
        if (sscanf(line, "%31s %d %d %d %d %d %d %d %d", desc, &cropx, &cropy,
                &cropw, &croph, &outw, &outh, &maxfps, &minfps) == 9 &&
            strchr(desc, 'x') && strchr(desc, '@')) {
            sensor_mode *m = &modes[count++];
            memset(m, 0, sizeof(*m));
            strncpy(m->desc, desc, sizeof(m->desc) - 1);
            m->crop_width = (unsigned short)cropw;
            m->crop_height = (unsigned short)croph;
            m->out_width = (unsigned short)outw;
            m->out_height = (unsigned short)outh;
            m->max_fps = (unsigned int)maxfps;
            m->min_fps = (unsigned int)minfps;
        }

        if (!eol)
            break;
        p = eol + 1;
    }
    return count;
}

int sensor_mode_read(int sensor_index, sensor_mode *modes, int max)
{
    char path[64];
    snprintf(path, sizeof(path),
        "/proc/mi_modules/mi_sensor/mi_sensor%d", sensor_index);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return sensor_mode_parse_proc(buf, modes, max);
}

void sensor_mode_log(const char *mod, const sensor_mode *modes, int count)
{
    HAL_INFO(mod, "Sensor exposes %d resolution mode(s):\n", count);
    for (int i = 0; i < count; i++)
        HAL_INFO(mod, "  [%d] %s: crop %ux%u, output %ux%u, fps %u-%u\n",
            i, modes[i].desc,
            modes[i].crop_width, modes[i].crop_height,
            modes[i].out_width, modes[i].out_height,
            modes[i].min_fps, modes[i].max_fps);
}

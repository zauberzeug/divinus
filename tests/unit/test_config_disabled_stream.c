/* Drives the real app_config_parse() to prove a disabled stream keeps its
   configured parameters across a load. Before the fix the parser only read the
   width/height/fps/... inside `if (<stream>_enable)`, so a disabled stream read
   back as zeros (mp4/jpeg, zero-initialized) or as the hardcoded reset defaults
   (mjpeg) — and, because the writer emits every field unconditionally, the next
   save persisted those bogus values, destroying the real config. */

#include <assert.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"

/* App globals app_config_parse() references but that live in modules we don't
   link here (region.c / the HAL). Provide them so the parser links standalone. */
hal_platform plat = HAL_PLATFORM_I6;
osd osds[MAX_OSD];
char timefmt[64];

static char conf_path[PATH_MAX];

/* The mandatory `system`/`isp` keys app_config_parse() aborts without. */
static const char *BASE =
    "system:\n"
    "  web_port: 80\n"
    "  web_enable_static: false\n"
    "  isp_thread_stack_size: 16384\n"
    "  venc_stream_thread_stack_size: 16384\n"
    "  web_server_thread_stack_size: 32768\n"
    "isp:\n"
    "  mirror: false\n"
    "  flip: false\n";

/* jpeg/mjpeg `enable` are mandatory (the parser aborts the load without them),
   so every config carries them; tests not focused on a section leave it OFF. */
#define JPEG_OFF "jpeg:\n  enable: false\n"
#define MJPEG_OFF "mjpeg:\n  enable: false\n"

/* Write BASE + the given stream block next to this executable — the first path
   app_config_open() probes — then run the real parser, CWD-independent. */
static void load(const char *streams) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    assert(n > 0);
    exe[n] = '\0';
    snprintf(conf_path, sizeof(conf_path), "%s/divinus.yaml", dirname(exe));
    FILE *f = fopen(conf_path, "w");
    assert(f);
    fputs(BASE, f);
    fputs(streams, f);
    fclose(f);
    assert(app_config_parse() == CONFIG_OK);
}

static void test_disabled_mp4_keeps_params(void) {
    load("mp4:\n  enable: false\n  width: 1920\n  height: 1080\n"
         "  fps: 30\n  gop: 60\n  bitrate: 4096\n  codec: H265\n"
         JPEG_OFF MJPEG_OFF);
    assert(!app_config.mp4_enable);
    assert(app_config.mp4_width == 1920);
    assert(app_config.mp4_height == 1080);
    assert(app_config.mp4_fps == 30);
    assert(app_config.mp4_gop == 60);
    assert(app_config.mp4_bitrate == 4096);
    assert(app_config.mp4_codecH265);
}

static void test_disabled_mjpeg_keeps_nondefault(void) {
    /* 1280 != the reset default 640 — proves the value came from the file. */
    load(JPEG_OFF
         "mjpeg:\n  enable: false\n  width: 1280\n  height: 720\n"
         "  fps: 25\n  bitrate: 2048\n");
    assert(!app_config.mjpeg_enable);
    assert(app_config.mjpeg_width == 1280);
    assert(app_config.mjpeg_height == 720);
    assert(app_config.mjpeg_fps == 25);
    assert(app_config.mjpeg_bitrate == 2048);
}

static void test_disabled_jpeg_keeps_params(void) {
    load("jpeg:\n  enable: false\n  width: 2560\n  height: 1440\n  qfactor: 85\n"
         MJPEG_OFF);
    assert(!app_config.jpeg_enable);
    assert(app_config.jpeg_width == 2560);
    assert(app_config.jpeg_height == 1440);
    assert(app_config.jpeg_qfactor == 85);
}

static void test_missing_mp4_section_uses_defaults_not_zero(void) {
    load(JPEG_OFF MJPEG_OFF);  /* no mp4 section at all */
    assert(!app_config.mp4_enable);
    assert(app_config.mp4_width == 1920);   /* the reset default, not 0 */
    assert(app_config.mp4_height == 1080);
    assert(app_config.mp4_fps == 30);
}

static void test_disabled_mp4_survives_save_reload(void) {
    /* The bug's actual mechanism: enable mp4 with distinctive params, disable
       it, then save + reload. The writer emits every param unconditionally, so
       a correct parser must read them back — previously the reload dropped them
       and the saved zeros destroyed the config. */
    load("mp4:\n  enable: true\n  width: 2304\n  height: 1296\n"
         "  fps: 24\n  gop: 48\n  bitrate: 3072\n" JPEG_OFF MJPEG_OFF);
    assert(app_config.mp4_enable && app_config.mp4_width == 2304);

    app_config.mp4_enable = false;             /* user turns the stream off */
    assert(app_config_save() == EXIT_SUCCESS); /* persists every param */
    assert(app_config_parse() == CONFIG_OK);

    assert(!app_config.mp4_enable);
    assert(app_config.mp4_width == 2304);      /* survives; was destroyed before */
    assert(app_config.mp4_height == 1296);
    assert(app_config.mp4_fps == 24);
    assert(app_config.mp4_gop == 48);
    assert(app_config.mp4_bitrate == 3072);
}

static void test_enabled_mp4_still_parses(void) {
    load("mp4:\n  enable: true\n  width: 3840\n  height: 2160\n"
         "  fps: 20\n  bitrate: 1024\n"
         JPEG_OFF MJPEG_OFF);
    assert(app_config.mp4_enable);
    assert(app_config.mp4_width == 3840);
    assert(app_config.mp4_height == 2160);
}

int main(void) {
    test_disabled_mp4_keeps_params();
    test_disabled_mjpeg_keeps_nondefault();
    test_disabled_jpeg_keeps_params();
    test_missing_mp4_section_uses_defaults_not_zero();
    test_disabled_mp4_survives_save_reload();
    test_enabled_mp4_still_parses();
    if (*conf_path) {
        char bak[PATH_MAX];
        snprintf(bak, sizeof(bak), "%s.bak", conf_path);  /* app_config_save() rotates one */
        remove(conf_path);
        remove(bak);
    }
    puts("test_config_disabled_stream: OK");
    return 0;
}

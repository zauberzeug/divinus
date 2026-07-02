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
static void write_config(const char *streams) {
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
}

static enum ConfigError parse_with_stderr_capture(char *logbuf, size_t size) {
    assert(size > 0);
    int saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);

    FILE *tmp = tmpfile();
    assert(tmp);
    assert(dup2(fileno(tmp), STDERR_FILENO) >= 0);

    enum ConfigError err = app_config_parse();

    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);

    assert(fseek(tmp, 0, SEEK_SET) == 0);
    size_t nread = fread(logbuf, 1, size - 1, tmp);
    logbuf[nread] = '\0';
    fclose(tmp);
    return err;
}

static void load(const char *streams) {
    write_config(streams);
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

static void test_disabled_http_post_keeps_params(void) {
    load("http_post:\n  enable: false\n  host: cam.local\n  url: /upload\n"
         "  login: u\n  password: p\n  width: 800\n  height: 600\n"
         "  interval: 5\n  qfactor: 70\n"
         JPEG_OFF MJPEG_OFF);
    assert(!app_config.http_post_enable);
    assert(strcmp(app_config.http_post_host, "cam.local") == 0);
    assert(strcmp(app_config.http_post_url, "/upload") == 0);
    assert(strcmp(app_config.http_post_login, "u") == 0);
    assert(strcmp(app_config.http_post_password, "p") == 0);
    assert(app_config.http_post_width == 800);
    assert(app_config.http_post_height == 600);
    assert(app_config.http_post_interval == 5);
    assert(app_config.http_post_qfactor == 70);
}

static void test_disabled_http_post_survives_save_reload(void) {
    /* The bug's actual mechanism: enable with distinctive params, disable,
       then save + reload. A correct parser must keep the disabled values. */
    load("http_post:\n  enable: true\n  host: save.local\n  url: /api/upload\n"
         "  login: saveu\n  password: savep\n  width: 1024\n  height: 768\n"
         "  interval: 9\n  qfactor: 61\n"
         JPEG_OFF MJPEG_OFF);
    assert(app_config.http_post_enable);
    assert(strcmp(app_config.http_post_host, "save.local") == 0);

    app_config.http_post_enable = false;       /* user turns the section off */
    assert(app_config_save() == EXIT_SUCCESS); /* persists every param */
    assert(app_config_parse() == CONFIG_OK);

    assert(!app_config.http_post_enable);
    assert(strcmp(app_config.http_post_host, "save.local") == 0);
    assert(strcmp(app_config.http_post_url, "/api/upload") == 0);
    assert(strcmp(app_config.http_post_login, "saveu") == 0);
    assert(strcmp(app_config.http_post_password, "savep") == 0);
    assert(app_config.http_post_width == 1024);
    assert(app_config.http_post_height == 768);
    assert(app_config.http_post_interval == 9);
    assert(app_config.http_post_qfactor == 61);
}

static void test_disabled_audio_keeps_params(void) {
    load("audio:\n  enable: false\n  bitrate: 256\n  gain: 12\n  srate: 44100\n"
         JPEG_OFF MJPEG_OFF);
    assert(!app_config.audio_enable);
    assert(app_config.audio_bitrate == 256);
    assert(app_config.audio_gain == 12);
    assert(app_config.audio_srate == 44100);
}

static void test_disabled_night_mode_keeps_params(void) {
    /* night_mode params are written unconditionally too; a disabled section
       must keep its configured pins/thresholds. Values differ from the reset
       defaults (pins 999, interval 10, delay 250, threshold 128, empty dev). */
    load("night_mode:\n  enable: false\n  ir_sensor_pin: 5\n"
         "  check_interval_s: 30\n  ir_cut_pin1: 6\n  ir_cut_pin2: 7\n"
         "  ir_led_pin: 8\n  pin_switch_delay_us: 500\n"
         "  adc_device: /dev/adc0\n  adc_threshold: 200\n"
         JPEG_OFF MJPEG_OFF);
    assert(!app_config.night_mode_enable);
    assert(app_config.ir_sensor_pin == 5);
    assert(app_config.check_interval_s == 30);
    assert(app_config.ir_cut_pin1 == 6);
    assert(app_config.ir_cut_pin2 == 7);
    assert(app_config.ir_led_pin == 8);
    assert(app_config.pin_switch_delay_us == 500);
    assert(strcmp(app_config.adc_device, "/dev/adc0") == 0);
    assert(app_config.adc_threshold == 200);
}

static void test_disabled_night_mode_unset_pins_are_silent(void) {
    char logbuf[4096];

    write_config("night_mode:\n  enable: false\n  ir_sensor_pin: 999\n"
                 "  check_interval_s: 30\n  ir_cut_pin1: 999\n"
                 "  ir_cut_pin2: 999\n  ir_led_pin: 999\n"
                 "  pin_switch_delay_us: 500\n"
                 "  adc_device: /dev/adc0\n  adc_threshold: 200\n"
                 JPEG_OFF MJPEG_OFF);

    assert(parse_with_stderr_capture(logbuf, sizeof(logbuf)) == CONFIG_OK);
    assert(!app_config.night_mode_enable);
    assert(app_config.ir_sensor_pin == 999);
    assert(app_config.ir_cut_pin1 == 999);
    assert(app_config.ir_cut_pin2 == 999);
    assert(app_config.ir_led_pin == 999);
    assert(strstr(logbuf, "not in a range") == NULL);
}

static void test_disabled_night_mode_out_of_range_pin_logs_error(void) {
    char logbuf[4096];

    write_config("night_mode:\n  enable: false\n  ir_sensor_pin: 200\n"
                 "  check_interval_s: 30\n  ir_cut_pin1: 999\n"
                 "  ir_cut_pin2: 999\n  ir_led_pin: 999\n"
                 "  pin_switch_delay_us: 500\n"
                 "  adc_device: /dev/adc0\n  adc_threshold: 200\n"
                 JPEG_OFF MJPEG_OFF);

    assert(parse_with_stderr_capture(logbuf, sizeof(logbuf)) == CONFIG_OK);
    assert(!app_config.night_mode_enable);
    assert(app_config.ir_sensor_pin == 999);
    assert(strstr(logbuf, "not in a range") != NULL);
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
    test_disabled_http_post_keeps_params();
    test_disabled_http_post_survives_save_reload();
    test_disabled_audio_keeps_params();
    test_disabled_night_mode_keeps_params();
    test_disabled_night_mode_unset_pins_are_silent();
    test_disabled_night_mode_out_of_range_pin_logs_error();
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

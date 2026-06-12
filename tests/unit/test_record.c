/* Host tests for record.c: output path composition and size-based segment
   rotation. Links the real fmt/ muxer; app_config and timefmt are defined
   here because app_config.c and region.c are not host-linkable. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "record.h"

struct AppConfig app_config;
char timefmt[64];

extern char recordOn, recordPath[256];

static char scratch[] = "/tmp/test_record_XXXXXX";

static void reset_config(const char *path, const char *filename) {
    memset(&app_config, 0, sizeof(app_config));
    snprintf(app_config.record_path, sizeof(app_config.record_path), "%s", path);
    snprintf(app_config.record_filename, sizeof(app_config.record_filename),
        "%s", filename);
    strcpy(timefmt, "%Y-%m-%d_%H-%M-%S");
}

static void test_configured_filename_lands_in_record_path(void) {
    reset_config(scratch, "out.mp4");

    record_start();
    assert(recordOn);

    char expected[300];
    snprintf(expected, sizeof(expected), "%s/out.mp4", scratch);
    assert(!strcmp(recordPath, expected));
    assert(!access(expected, F_OK));

    record_stop();
    assert(!unlink(expected));
}

static void test_trailing_slash_does_not_double(void) {
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/", scratch);
    reset_config(dir, "out.mp4");

    record_start();
    assert(recordOn);

    char expected[300];
    snprintf(expected, sizeof(expected), "%sout.mp4", dir);
    assert(!strcmp(recordPath, expected));

    record_stop();
    assert(!unlink(expected));
}

static void test_auto_name_lands_in_record_path(void) {
    reset_config(scratch, "");
    strcpy(timefmt, "%Y");

    record_start();
    assert(recordOn);

    time_t now = time(NULL);
    struct tm tm_buf;
    char year[8], expected[300];
    strftime(year, sizeof(year), "%Y", localtime_r(&now, &tm_buf));
    snprintf(expected, sizeof(expected), "%s/recording_%s.mp4", scratch, year);
    assert(!strcmp(recordPath, expected));
    assert(!access(expected, F_OK));

    record_stop();
    assert(!unlink(expected));
}

static void test_missing_dir_fails_with_clear_error(void) {
    char dir[80];
    snprintf(dir, sizeof(dir), "%s/missing", scratch);
    reset_config(dir, "out.mp4");

    char errfile[300];
    snprintf(errfile, sizeof(errfile), "%s/stderr.txt", scratch);
    fflush(stderr);
    int saved = dup(2);
    assert(saved >= 0);
    FILE *redir = freopen(errfile, "w", stderr);
    assert(redir);
    record_start();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);

    assert(!recordOn);

    char captured[512] = {0};
    FILE *f = fopen(errfile, "r");
    assert(f);
    assert(fread(captured, 1, sizeof(captured) - 1, f) > 0);
    fclose(f);
    assert(strstr(captured, dir)); /* error names the failing path */
    assert(!unlink(errfile));
}

int main(void) {
    assert(mkdtemp(scratch));
    assert(!chdir(scratch)); /* keep stray CWD writes out of the repo */

    test_configured_filename_lands_in_record_path();
    test_trailing_slash_does_not_double();
    test_auto_name_lands_in_record_path();
    test_missing_dir_fails_with_clear_error();

    assert(!rmdir(scratch));
    puts("test_record: OK");
    return 0;
}

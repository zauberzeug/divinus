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

static void add_nal(unsigned char *data, unsigned int *len, hal_vidpack *pack,
    int type, const unsigned char *payload, unsigned int payload_len) {
    static const unsigned char startcode[4] = {0, 0, 0, 1};
    pack->nalu[pack->naluCnt].offset = *len;
    pack->nalu[pack->naluCnt].length = payload_len + 4;
    pack->nalu[pack->naluCnt].type = type;
    pack->naluCnt++;
    memcpy(data + *len, startcode, 4);
    *len += 4;
    memcpy(data + *len, payload, payload_len);
    *len += payload_len;
}

static void feed_pack(char idr) {
    static const unsigned char sps[] = {0x67, 0x42, 0x00, 0x28, 0x96, 0x35};
    static const unsigned char pps[] = {0x68, 0xce, 0x38, 0x80};
    unsigned char slice[600], data[4096];
    unsigned int len = 0;
    hal_vidpack pack = {0};

    memset(slice, 0xab, sizeof(slice));
    slice[0] = idr ? 0x65 : 0x41;
    if (idr) {
        add_nal(data, &len, &pack, NalUnitType_SPS, sps, sizeof(sps));
        add_nal(data, &len, &pack, NalUnitType_PPS, pps, sizeof(pps));
    }
    add_nal(data, &len, &pack,
        idr ? NalUnitType_CodedSliceIdr : NalUnitType_CodedSliceNonIdr,
        slice, sizeof(slice));
    pack.data = data;
    pack.length = len;

    hal_vidstream stream = {.pack = &pack, .count = 1};
    send_mp4_to_record(&stream, 0);
}

/* A playable segment is ftyp, moov, then whole moof+mdat pairs, with the box
   sizes consuming the file exactly. */
static void assert_valid_segment(const char *path, int expect_moofs) {
    struct stat st;
    assert(!stat(path, &st));
    assert(st.st_size > 0);

    unsigned char *data = malloc(st.st_size);
    assert(data);
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fread(data, 1, st.st_size, f) == (size_t)st.st_size);
    fclose(f);

    long pos = 0;
    int moofs = 0;
    char seen_moov = 0, expect_mdat = 0;
    while (pos < st.st_size) {
        assert(pos + 8 <= st.st_size);
        long box_len = ((long)data[pos] << 24) | (data[pos + 1] << 16) |
                       (data[pos + 2] << 8) | data[pos + 3];
        const unsigned char *type = data + pos + 4;
        if (pos == 0)
            assert(!memcmp(type, "ftyp", 4));
        if (expect_mdat) {
            assert(!memcmp(type, "mdat", 4));
            expect_mdat = 0;
        }
        if (!memcmp(type, "moov", 4))
            seen_moov = 1;
        if (!memcmp(type, "moof", 4)) {
            assert(seen_moov);
            expect_mdat = 1;
            moofs++;
        }
        assert(box_len >= 8 && pos + box_len <= st.st_size);
        pos += box_len;
    }
    assert(pos == st.st_size);
    assert(!expect_mdat);
    assert(moofs == expect_moofs);
    free(data);
}

static void test_no_rotation_below_size_threshold(void) {
    reset_config(scratch, "out.mp4");
    app_config.record_segment_size = 10 * 1024 * 1024;

    record_start();
    assert(recordOn);
    feed_pack(1);
    feed_pack(0);
    feed_pack(0);
    record_stop();

    char path[300];
    snprintf(path, sizeof(path), "%s/out.mp4", scratch);
    assert_valid_segment(path, 3);
    assert(!unlink(path));
}

static void test_size_rotation_only_on_fragment_boundaries(void) {
    reset_config(scratch, "");
    app_config.record_segment_size = 1; /* rotate after every fragment */

    record_start();
    assert(recordOn);

    /* Rename the open segment aside after each rotation so every segment
       survives strftime name collisions and can be inspected afterwards. */
    char seg[3][300];
    for (int i = 0; i < 3; i++) {
        snprintf(seg[i], sizeof(seg[i]), "%s/seg%d.mp4", scratch, i);
        assert(!rename(recordPath, seg[i]));
        feed_pack(i == 0);
    }

    char last[300];
    snprintf(last, sizeof(last), "%s", recordPath);
    record_stop();

    for (int i = 0; i < 3; i++) {
        assert_valid_segment(seg[i], 1);
        assert(!unlink(seg[i]));
    }

    /* The rotation opened the next segment which stop() left empty. */
    struct stat st;
    assert(!stat(last, &st));
    assert(st.st_size == 0);
    assert(!unlink(last));
}

int main(void) {
    assert(mkdtemp(scratch));
    assert(!chdir(scratch)); /* keep stray CWD writes out of the repo */

    test_configured_filename_lands_in_record_path();
    test_trailing_slash_does_not_double();
    test_auto_name_lands_in_record_path();
    test_missing_dir_fails_with_clear_error();
    test_no_rotation_below_size_threshold();
    test_size_rotation_only_on_fragment_boundaries();

    assert(!rmdir(scratch));
    puts("test_record: OK");
    return 0;
}

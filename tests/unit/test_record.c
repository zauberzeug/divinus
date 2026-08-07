/* Host tests for record.c: output path composition and size-based segment
   rotation. Links the real fmt/ muxer; app_config and timefmt are defined
   here because app_config.c and region.c are not host-linkable. */

#include <assert.h>
#include <pthread.h>
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

typedef struct SinkGate {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    char blocked;
    int blocked_writers;
    unsigned int write_calls;
    useconds_t write_delay_us;
} SinkGate;

typedef struct ProducerCtx {
    int total_feeds;
    int feeds_done;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
} ProducerCtx;

static SinkGate *active_sink_gate;

static long monotonic_ns(void) {
    struct timespec ts;
    assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void sink_gate_init(SinkGate *gate, char blocked, useconds_t delay_us) {
    assert(!pthread_mutex_init(&gate->mtx, NULL));
    assert(!pthread_cond_init(&gate->cond, NULL));
    gate->blocked = blocked;
    gate->blocked_writers = 0;
    gate->write_calls = 0;
    gate->write_delay_us = delay_us;
}

static void sink_gate_destroy(SinkGate *gate) {
    assert(!pthread_cond_destroy(&gate->cond));
    assert(!pthread_mutex_destroy(&gate->mtx));
}

static void sink_gate_wait_until_blocked(SinkGate *gate) {
    assert(!pthread_mutex_lock(&gate->mtx));
    while (gate->blocked_writers == 0) {
        assert(!pthread_cond_wait(&gate->cond, &gate->mtx));
    }
    assert(!pthread_mutex_unlock(&gate->mtx));
}

static void sink_gate_release(SinkGate *gate) {
    assert(!pthread_mutex_lock(&gate->mtx));
    gate->blocked = 0;
    assert(!pthread_cond_broadcast(&gate->cond));
    assert(!pthread_mutex_unlock(&gate->mtx));
}

static void *gated_sink_open(const char *path) {
    return fopen(path, "wb");
}

static ssize_t gated_sink_write(void *handle, const void *data, size_t len) {
    FILE *out = handle;
    SinkGate *gate = active_sink_gate;

    assert(gate);
    assert(!pthread_mutex_lock(&gate->mtx));
    gate->write_calls++;
    if (gate->blocked) {
        gate->blocked_writers++;
        assert(!pthread_cond_broadcast(&gate->cond));
        while (gate->blocked) {
            assert(!pthread_cond_wait(&gate->cond, &gate->mtx));
        }
        gate->blocked_writers--;
    }
    useconds_t delay = gate->write_delay_us;
    assert(!pthread_mutex_unlock(&gate->mtx));

    if (delay) usleep(delay);

    if (fwrite(data, 1, len, out) == len) return (ssize_t)len;
    return -1;
}

static int gated_sink_sync(void *handle) {
    return fsync(fileno((FILE *)handle));
}

static int gated_sink_close(void *handle) {
    return fclose((FILE *)handle);
}

static const RecSink gated_sink = {
    .open = gated_sink_open,
    .write = gated_sink_write,
    .sync = gated_sink_sync,
    .close = gated_sink_close,
};

/* Rotation-ordering guard. A rotated segment must open with its own ftyp/moov
   header; if in-flight fragments reach the newly-opened file first (the pre-fix
   async header-reset bug) the segment head is an orphan moof. This sink flags any
   segment whose first write is not an ftyp box, and runs slow so fragments queue
   across the rotation window. */
static int trace_segments_opened;
static int trace_order_violations;
static int trace_expect_header;

static void *trace_sink_open(const char *path) {
    trace_expect_header = 1;
    trace_segments_opened++;
    return fopen(path, "wb");
}

static ssize_t trace_sink_write(void *handle, const void *data, size_t len) {
    const unsigned char *d = data;

    if (trace_expect_header) {
        if (len < 8 || memcmp(d + 4, "ftyp", 4) != 0) trace_order_violations++;
        trace_expect_header = 0;
    }

    usleep(1500);   /* lag the writer so fragments pile up during rotation */
    if (fwrite(data, 1, len, (FILE *)handle) == len) return (ssize_t)len;
    return -1;
}

static int trace_sink_sync(void *handle) {
    return fsync(fileno((FILE *)handle));
}

static int trace_sink_close(void *handle) {
    return fclose((FILE *)handle);
}

static const RecSink trace_sink = {
    .open = trace_sink_open,
    .write = trace_sink_write,
    .sync = trace_sink_sync,
    .close = trace_sink_close,
};

static int count_moofs_in_valid_segment(const char *path) {
    struct stat st;
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fstat(fileno(f), &st));   /* size from the open fd: no check-then-reopen race */
    assert(st.st_size > 0);

    unsigned char *data = malloc(st.st_size);
    assert(data);
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
    free(data);
    return moofs;
}

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
    assert(count_moofs_in_valid_segment(path) == expect_moofs);
}

/* Frames arriving before the first IDR (and its SPS/PPS) must not be
   written: the muxer has no header yet, so the segment would start with
   bare moof boxes and be unplayable. Must run before any test feeds an
   IDR, while the muxer globals are still pristine as at process start. */
static void test_no_fragments_before_first_idr(void) {
    reset_config(scratch, "out.mp4");

    record_start();
    assert(recordOn);
    feed_pack(0);
    feed_pack(0);

    char path[300];
    snprintf(path, sizeof(path), "%s/out.mp4", scratch);
    struct stat st;
    assert(!stat(path, &st));
    assert(st.st_size == 0); /* nothing until the header exists */

    feed_pack(1);
    record_stop();
    assert_valid_segment(path, 1);
    assert(!unlink(path));
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
        record_flush(); /* observe the writer's rotation deterministically */
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


static void test_writer_non_blocking_enqueue(void) {
    SinkGate gate;
    char path[300];
    long t0;
    long elapsed_ns;

    reset_config(scratch, "out.mp4");
    sink_gate_init(&gate, 1, 0);
    active_sink_gate = &gate;
    record_set_sink(&gated_sink);

    record_start();
    assert(recordOn);

    feed_pack(1);
    sink_gate_wait_until_blocked(&gate);

    t0 = monotonic_ns();
    for (int i = 0; i < 6; i++) {
        feed_pack(0);
    }
    elapsed_ns = monotonic_ns() - t0;
    assert(elapsed_ns < 200L * 1000L * 1000L);

    sink_gate_release(&gate);
    record_flush();
    record_stop();

    snprintf(path, sizeof(path), "%s/out.mp4", scratch);
    assert_valid_segment(path, 7);
    assert(gate.write_calls >= 8);
    assert(!unlink(path));

    record_set_sink(NULL);
    active_sink_gate = NULL;
    sink_gate_destroy(&gate);
    puts("  writer: non-blocking enqueue: OK");
}

static void test_writer_overflow_drops_oldest_keeps_valid_file(void) {
    SinkGate gate;
    char path[300];
    size_t old_cap = record_queue_cap_bytes;
    long t0;
    long elapsed_ns;
    int moofs;

    reset_config(scratch, "out.mp4");
    sink_gate_init(&gate, 1, 0);
    active_sink_gate = &gate;
    record_set_sink(&gated_sink);
    record_set_queue_cap(4096);

    record_start();
    assert(recordOn);

    feed_pack(1);
    sink_gate_wait_until_blocked(&gate);

    t0 = monotonic_ns();
    for (int i = 0; i < 40; i++) {
        feed_pack(0);
    }
    elapsed_ns = monotonic_ns() - t0;
    assert(elapsed_ns < 400L * 1000L * 1000L);

    sink_gate_release(&gate);
    record_flush();
    record_stop();

    snprintf(path, sizeof(path), "%s/out.mp4", scratch);
    moofs = count_moofs_in_valid_segment(path);
    assert(moofs > 0);
    assert(moofs < 41);
    assert_valid_segment(path, moofs);
    assert(!unlink(path));

    record_set_queue_cap(old_cap);
    record_set_sink(NULL);
    active_sink_gate = NULL;
    sink_gate_destroy(&gate);
    puts("  writer: overflow drop-oldest valid segment: OK");
}

static void *producer_thread_main(void *arg) {
    ProducerCtx *ctx = arg;

    for (int i = 0; i < ctx->total_feeds; i++) {
        feed_pack(0);
        assert(!pthread_mutex_lock(&ctx->mtx));
        ctx->feeds_done++;
        assert(!pthread_cond_broadcast(&ctx->cond));
        assert(!pthread_mutex_unlock(&ctx->mtx));
    }
    return NULL;
}

static void test_writer_stop_drains_cleanly_race(void) {
    SinkGate gate;
    ProducerCtx ctx = {
        .total_feeds = 200,
        .feeds_done = 0,
        .mtx = PTHREAD_MUTEX_INITIALIZER,
        .cond = PTHREAD_COND_INITIALIZER,
    };
    pthread_t producer;
    char path[300];
    int moofs;

    reset_config(scratch, "out.mp4");
    sink_gate_init(&gate, 0, 2000);
    active_sink_gate = &gate;
    record_set_sink(&gated_sink);

    record_start();
    assert(recordOn);
    feed_pack(1);

    assert(!pthread_create(&producer, NULL, producer_thread_main, &ctx));
    assert(!pthread_mutex_lock(&ctx.mtx));
    while (ctx.feeds_done < 20) {
        assert(!pthread_cond_wait(&ctx.cond, &ctx.mtx));
    }
    assert(!pthread_mutex_unlock(&ctx.mtx));

    record_stop();
    assert(!recordOn);
    assert(!pthread_join(producer, NULL));

    snprintf(path, sizeof(path), "%s/out.mp4", scratch);
    moofs = count_moofs_in_valid_segment(path);
    assert(moofs >= 1);
    assert_valid_segment(path, moofs);
    assert(!unlink(path));

    record_set_sink(NULL);
    active_sink_gate = NULL;
    sink_gate_destroy(&gate);
    assert(!pthread_cond_destroy(&ctx.cond));
    assert(!pthread_mutex_destroy(&ctx.mtx));
    puts("  writer: stop drains cleanly: OK");
}

static void test_writer_rotation_header_leads_each_segment(void) {
    char path[300];

    reset_config(scratch, "");            /* auto-name */
    app_config.record_segment_size = 1;   /* rotate on every fragment */

    trace_segments_opened = 0;
    trace_order_violations = 0;
    trace_expect_header = 0;
    active_sink_gate = NULL;
    record_set_sink(&trace_sink);

    record_start();
    assert(recordOn);

    /* Feed many fragments with NO per-fragment flush, so fragments queue while
       the slow sink is mid-rotation -- the window the pre-fix bug corrupted. */
    feed_pack(1);
    for (int i = 0; i < 24; i++) feed_pack(0);

    record_flush();
    record_stop();

    assert(trace_segments_opened >= 3);    /* rotation actually happened repeatedly */
    assert(trace_order_violations == 0);   /* every segment opened with its ftyp header */

    record_set_sink(NULL);
    snprintf(path, sizeof(path), "%s", recordPath);
    unlink(path);
    puts("  writer: rotation header leads each segment: OK");
}

int main(void) {
    assert(mkdtemp(scratch));
    assert(!chdir(scratch)); /* keep stray CWD writes out of the repo */

    test_configured_filename_lands_in_record_path();
    test_trailing_slash_does_not_double();
    test_auto_name_lands_in_record_path();
    test_missing_dir_fails_with_clear_error();
    test_no_fragments_before_first_idr();
    test_no_rotation_below_size_threshold();
    test_size_rotation_only_on_fragment_boundaries();
    test_writer_non_blocking_enqueue();
    test_writer_overflow_drops_oldest_keeps_valid_file();
    test_writer_stop_drains_cleanly_race();
    test_writer_rotation_header_leads_each_segment();

    assert(!rmdir(scratch));
    puts("test_record: OK");
    return 0;
}

#include "record.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define REC_DEFAULT_QUEUE_CAP_BYTES (4u * 1024u * 1024u)
#define REC_STOP_DRAIN_TIMEOUT_SEC 10

typedef struct RecSeg {
    uint8_t *data;
    size_t len;
    bool sticky;
    struct RecSeg *next;
} RecSeg;

typedef enum RecKind {
    REC_OPEN,
    REC_DATA,
    REC_BARRIER,
    REC_ROTATE,
    REC_STOP,
    REC_QUIT
} RecKind;

typedef struct RecBarrier {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    bool done;
    int refs;    /* freed when the last of {waiter, writer/queue} releases it */
} RecBarrier;

typedef struct RecMsg {
    RecKind kind;
    void *file;
    RecSeg *seg;
    RecBarrier *barrier;
    char path[256];
    struct RecMsg *next;
} RecMsg;

extern pthread_mutex_t mp4Mtx __attribute__((weak));

static pthread_mutex_t rec_fallback_mp4_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rec_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_q_cond = PTHREAD_COND_INITIALIZER;
static RecMsg *rec_head;
static RecMsg *rec_tail;
static size_t rec_q_bytes;
static bool rec_overflow_logged;
static bool rec_writer_running;
static bool rec_cleanup_registered;
static pthread_t rec_writer_tid;

static struct Mp4State recordState;
time_t recordStartTime = 0;
char recordOn = 0, recordPath[256];
size_t record_queue_cap_bytes = REC_DEFAULT_QUEUE_CAP_BYTES;
static size_t rec_venc_seg_bytes;   /* venc-thread only: bytes in the open segment */
static time_t rec_venc_seg_start;   /* venc-thread only: open segment's start time */

static void *rec_sink_open_default(const char *path) {
    return fopen(path, "wb");
}

static ssize_t rec_sink_write_default(void *handle, const void *data, size_t len) {
    FILE *file = handle;
    size_t written = fwrite(data, 1, len, file);

    if (written == len) return (ssize_t)written;
    return -1;
}

static int rec_sink_sync_default(void *handle) {
    FILE *file = handle;

    return fsync(fileno(file));
}

static int rec_sink_close_default(void *handle) {
    FILE *file = handle;

    return fclose(file);
}

static RecSink rec_sink = {
    .open = rec_sink_open_default,
    .write = rec_sink_write_default,
    .sync = rec_sink_sync_default,
    .close = rec_sink_close_default,
};

static pthread_mutex_t *rec_mp4_mutex(void) {
    if (&mp4Mtx) return &mp4Mtx;
    return &rec_fallback_mp4_mtx;
}

static void rec_lock_mp4(void) {
    pthread_mutex_lock(rec_mp4_mutex());
}

static void rec_unlock_mp4(void) {
    pthread_mutex_unlock(rec_mp4_mutex());
}

static void rec_seg_free(RecSeg *seg) {
    if (!seg) return;

    free(seg->data);
    free(seg);
}

static void rec_msg_free(RecMsg *msg) {
    if (!msg) return;

    rec_seg_free(msg->seg);
    free(msg);
}

static RecMsg *rec_msg_new(RecKind kind) {
    RecMsg *msg = calloc(1, sizeof(*msg));

    if (!msg) return NULL;
    msg->kind = kind;
    return msg;
}

static RecMsg *rec_msg_open(void *file) {
    RecMsg *msg = rec_msg_new(REC_OPEN);

    if (!msg) return NULL;
    msg->file = file;
    return msg;
}

static RecMsg *rec_msg_barrier(RecBarrier *barrier) {
    RecMsg *msg = rec_msg_new(REC_BARRIER);

    if (!msg) return NULL;
    msg->barrier = barrier;
    return msg;
}

static RecMsg *rec_msg_stop(RecBarrier *barrier) {
    RecMsg *msg = rec_msg_new(REC_STOP);

    if (!msg) return NULL;
    msg->barrier = barrier;
    return msg;
}

static RecMsg *rec_msg_quit(void) {
    return rec_msg_new(REC_QUIT);
}

static RecMsg *rec_msg_rotate(const char *path) {
    RecMsg *msg = rec_msg_new(REC_ROTATE);

    if (!msg) return NULL;
    strncpy(msg->path, path, sizeof(msg->path) - 1);
    return msg;
}

static RecMsg *rec_msg_data_concat(const uint8_t *first, size_t first_len,
    const uint8_t *second, size_t second_len, bool sticky) {
    RecMsg *msg = rec_msg_new(REC_DATA);
    RecSeg *seg;
    uint8_t *buf;

    if (!msg) return NULL;

    seg = calloc(1, sizeof(*seg));
    if (!seg) {
        free(msg);
        return NULL;
    }

    buf = malloc(first_len + second_len);
    if (!buf) {
        free(seg);
        free(msg);
        return NULL;
    }

    if (first_len) memcpy(buf, first, first_len);
    if (second_len) memcpy(buf + first_len, second, second_len);

    seg->data = buf;
    seg->len = first_len + second_len;
    seg->sticky = sticky;
    msg->seg = seg;

    return msg;
}

static size_t rec_msg_bytes(const RecMsg *msg) {
    if (!msg || msg->kind != REC_DATA || !msg->seg) return 0;
    return msg->seg->len;
}

static bool rec_drop_oldest_non_sticky_locked(void) {
    RecMsg *msg = rec_head;
    RecMsg *prev = NULL;

    while (msg) {
        if (msg->kind == REC_DATA && msg->seg && !msg->seg->sticky) break;
        prev = msg;
        msg = msg->next;
    }

    if (!msg) return false;

    if (prev) prev->next = msg->next;
    else rec_head = msg->next;

    if (rec_tail == msg) rec_tail = prev;

    rec_q_bytes -= rec_msg_bytes(msg);
    rec_msg_free(msg);
    return true;
}

static bool rec_enqueue(RecMsg *msg) {
    bool sticky = false;
    size_t bytes;
    int dropped = 0;
    bool overflow = false;

    if (!msg) return false;

    bytes = rec_msg_bytes(msg);
    if (msg->kind == REC_DATA && msg->seg) sticky = msg->seg->sticky;

    pthread_mutex_lock(&rec_q_mtx);

    if (bytes > 0 && !sticky) {
        while (rec_q_bytes + bytes > record_queue_cap_bytes) {
            overflow = true;
            if (!rec_drop_oldest_non_sticky_locked()) {
                break;
            }
            dropped++;
        }

        if (rec_q_bytes + bytes > record_queue_cap_bytes) {
            if (!rec_overflow_logged) {
                HAL_WARNING("record",
                    "queue full (%zu/%zu), dropped %d fragment(s)\n",
                    rec_q_bytes, record_queue_cap_bytes,
                    dropped > 0 ? dropped : 1);
                rec_overflow_logged = true;
            }
            pthread_mutex_unlock(&rec_q_mtx);
            rec_msg_free(msg);
            return false;
        }
    }

    if (!overflow) rec_overflow_logged = false;
    else if (dropped > 0 && !rec_overflow_logged) {
        HAL_WARNING("record",
            "queue full (%zu/%zu), dropped %d fragment(s)\n",
            rec_q_bytes, record_queue_cap_bytes, dropped);
        rec_overflow_logged = true;
    }

    msg->next = NULL;
    if (rec_tail) rec_tail->next = msg;
    else rec_head = msg;
    rec_tail = msg;
    rec_q_bytes += bytes;

    pthread_cond_signal(&rec_q_cond);
    pthread_mutex_unlock(&rec_q_mtx);

    return true;
}

static RecMsg *rec_dequeue(void) {
    RecMsg *msg;

    pthread_mutex_lock(&rec_q_mtx);
    while (!rec_head) {
        pthread_cond_wait(&rec_q_cond, &rec_q_mtx);
    }

    msg = rec_head;
    rec_head = msg->next;
    if (!rec_head) rec_tail = NULL;

    rec_q_bytes -= rec_msg_bytes(msg);
    if (rec_q_bytes < record_queue_cap_bytes) rec_overflow_logged = false;

    pthread_mutex_unlock(&rec_q_mtx);

    msg->next = NULL;
    return msg;
}

static bool rec_build_path(time_t now, char *out, size_t out_len) {
    char file_name[128];

    if (EMPTY(app_config.record_path)) return false;

    out[0] = '\0';
    strncpy(out, app_config.record_path, out_len - 1);
    out[out_len - 1] = '\0';

    if (out[strlen(out) - 1] != '/') {
        strncat(out, "/", out_len - strlen(out) - 1);
    }

    if (!EMPTY(app_config.record_filename)) {
        strncpy(file_name, app_config.record_filename, sizeof(file_name) - 1);
        file_name[sizeof(file_name) - 1] = '\0';
    } else {
        char temp_name[160];
        struct tm tm_buf;
        struct tm *tm_info = localtime_r(&now, &tm_buf);

        if (!tm_info) return false;

        snprintf(temp_name, sizeof(temp_name), "recording_%s.mp4", timefmt);
        if (!strftime(file_name, sizeof(file_name), temp_name, tm_info)) {
            file_name[0] = '\0';
        }
    }

    strncat(out, file_name, out_len - strlen(out) - 1);
    return true;
}

static void rec_sync_and_close(void *handle) {
    if (!handle) return;

    if (rec_sink.sync(handle) != 0) {
        HAL_WARNING("record", "failed to sync recording sink: %s\n", strerror(errno));
    }

    if (rec_sink.close(handle) != 0) {
        HAL_WARNING("record", "failed to close recording sink: %s\n", strerror(errno));
    }
}

static RecBarrier *rec_barrier_alloc(void) {
    RecBarrier *b = calloc(1, sizeof(*b));

    if (!b) return NULL;
    if (pthread_mutex_init(&b->mtx, NULL) != 0) {
        free(b);
        return NULL;
    }
    if (pthread_cond_init(&b->cond, NULL) != 0) {
        pthread_mutex_destroy(&b->mtx);
        free(b);
        return NULL;
    }
    b->done = false;
    b->refs = 2;    /* one ref for the waiter, one for the writer/queue */
    return b;
}

static void rec_barrier_destroy(RecBarrier *barrier) {
    pthread_mutex_destroy(&barrier->mtx);
    pthread_cond_destroy(&barrier->cond);
    free(barrier);
}

static void rec_barrier_release(RecBarrier *barrier) {
    int refs;

    if (!barrier) return;

    pthread_mutex_lock(&barrier->mtx);
    refs = --barrier->refs;
    pthread_mutex_unlock(&barrier->mtx);

    if (refs == 0)
        rec_barrier_destroy(barrier);
}

static void rec_barrier_signal(RecBarrier *barrier) {
    if (!barrier) return;

    pthread_mutex_lock(&barrier->mtx);
    barrier->done = true;
    pthread_cond_signal(&barrier->cond);
    pthread_mutex_unlock(&barrier->mtx);

    rec_barrier_release(barrier);   /* release the writer/queue ref */
}

static bool rec_should_rotate(size_t segment_bytes, time_t segment_start) {
    if (app_config.record_segment_size > 0 &&
        segment_bytes >= (size_t)app_config.record_segment_size) {
        return true;
    }

    if (app_config.record_segment_duration > 0 && segment_start > 0) {
        time_t now = time(NULL);

        if (now != (time_t)-1 && now - segment_start >= app_config.record_segment_duration) {
            return true;
        }
    }

    return false;
}

static void *rec_writer_main(void *arg) {
    void *out = NULL;

    (void)arg;

    for (;;) {
        RecMsg *msg = rec_dequeue();

        switch (msg->kind) {
            case REC_OPEN:
                if (out) rec_sync_and_close(out);
                out = msg->file;
                break;
            case REC_DATA:
                if (!out || !msg->seg || !msg->seg->data || !msg->seg->len) break;

                if (rec_sink.write(out, msg->seg->data, msg->seg->len) < 0) {
                    HAL_WARNING("record", "failed to write recording data: %s\n", strerror(errno));
                    rec_sync_and_close(out);
                    out = NULL;
                }
                break;
            case REC_ROTATE:
                /* The venc thread already cut the muxer (fresh ftyp/moov + zeroed
                   timeline for the next fragment) and enqueued this ROTATE ahead of
                   that header, so queue order is [old frags..., ROTATE, header, new
                   frags...] and every segment file opens with its own header. */
                if (out) rec_sync_and_close(out);
                out = rec_sink.open(msg->path);
                if (!out)
                    HAL_DANGER("record", "Failed to open the destination file %s: %s\n",
                        msg->path, strerror(errno));
                break;
            case REC_BARRIER:
                rec_barrier_signal(msg->barrier);
                break;
            case REC_STOP:
                if (out) {
                    rec_sync_and_close(out);
                    out = NULL;
                }
                rec_barrier_signal(msg->barrier);
                break;
            case REC_QUIT:
                if (out) rec_sync_and_close(out);
                rec_msg_free(msg);
                goto done;
        }

        rec_msg_free(msg);
    }

done:
    pthread_mutex_lock(&rec_q_mtx);
    rec_writer_running = false;
    pthread_mutex_unlock(&rec_q_mtx);

    return NULL;
}

static bool rec_writer_start(void) {
    int ret;

    pthread_mutex_lock(&rec_q_mtx);
    if (rec_writer_running) {
        pthread_mutex_unlock(&rec_q_mtx);
        return true;
    }

    ret = pthread_create(&rec_writer_tid, NULL, rec_writer_main, NULL);
    if (ret != 0) {
        pthread_mutex_unlock(&rec_q_mtx);
        HAL_DANGER("record", "Failed to create writer thread: %s\n", strerror(ret));
        return false;
    }

    rec_writer_running = true;
    pthread_mutex_unlock(&rec_q_mtx);
    return true;
}

static void rec_drain_queue(void) {
    while (rec_head) {
        RecMsg *msg = rec_head;

        rec_head = msg->next;
        /* Wake any waiter blocked on a control message we are discarding, so a
           post-join record_stop/record_flush cannot hang forever. */
        if (msg->kind == REC_STOP || msg->kind == REC_BARRIER)
            rec_barrier_signal(msg->barrier);
        rec_msg_free(msg);
    }

    rec_tail = NULL;
    rec_q_bytes = 0;
}

static void rec_writer_join(void) {
    RecMsg *quit_msg;
    bool running;

    pthread_mutex_lock(&rec_q_mtx);
    running = rec_writer_running;
    pthread_mutex_unlock(&rec_q_mtx);

    if (!running) return;

    quit_msg = rec_msg_quit();
    if (!quit_msg || !rec_enqueue(quit_msg)) {
        rec_msg_free(quit_msg);
        HAL_DANGER("record", "Failed to enqueue REC_QUIT message\n");
        return;
    }

    pthread_mutex_lock(&rec_q_mtx);
    pthread_cond_broadcast(&rec_q_cond);
    pthread_mutex_unlock(&rec_q_mtx);

    pthread_join(rec_writer_tid, NULL);

    pthread_mutex_lock(&rec_q_mtx);
    rec_drain_queue();
    pthread_mutex_unlock(&rec_q_mtx);
}

static void rec_cleanup(void) {
    rec_writer_join();
}

void record_set_sink(const RecSink *sink) {
    /* Test-only seam. Join the writer first so swapping the sink cannot race a
       live write; the next record_start() respawns it with the new sink.
       Callers swap only while recording is stopped. */
    rec_writer_join();

    pthread_mutex_lock(&rec_q_mtx);
    if (sink) rec_sink = *sink;
    else {
        rec_sink.open = rec_sink_open_default;
        rec_sink.write = rec_sink_write_default;
        rec_sink.sync = rec_sink_sync_default;
        rec_sink.close = rec_sink_close_default;
    }

    pthread_mutex_unlock(&rec_q_mtx);
}

void record_set_queue_cap(size_t cap_bytes) {
    pthread_mutex_lock(&rec_q_mtx);
    if (cap_bytes > 0) record_queue_cap_bytes = cap_bytes;
    else record_queue_cap_bytes = REC_DEFAULT_QUEUE_CAP_BYTES;
    pthread_mutex_unlock(&rec_q_mtx);
}

void record_start(void) {
    RecMsg *open_msg;
    void *opened_file;
    char path[sizeof(recordPath)];
    time_t start = time(NULL);

    /* Build the path and open the file BEFORE taking mp4Mtx: a create/open on
       FAT/SD can stall tens-to-hundreds of ms (directory allocation, card GC),
       and the venc fan-out thread takes mp4Mtx on every frame. */
    if (!rec_build_path(start, path, sizeof(path))) {
        HAL_DANGER("record", "Destination path is not set!\n");
        return;
    }

    opened_file = rec_sink.open(path);
    if (!opened_file) {
        HAL_DANGER("record", "Failed to open the destination file %s: %s\n",
            path, strerror(errno));
        return;
    }

    rec_lock_mp4();

    if (recordOn) {
        rec_unlock_mp4();
        rec_sync_and_close(opened_file);
        return;
    }

    if (!rec_writer_start()) {
        rec_unlock_mp4();
        rec_sync_and_close(opened_file);
        return;
    }

    open_msg = rec_msg_open(opened_file);
    if (!open_msg || !rec_enqueue(open_msg)) {
        rec_msg_free(open_msg);
        rec_unlock_mp4();
        rec_sync_and_close(opened_file);
        HAL_DANGER("record", "Failed to enqueue recording open message\n");
        return;
    }

    recordState.header_sent = false;
    recordStartTime = start;
    strncpy(recordPath, path, sizeof(recordPath) - 1);
    recordPath[sizeof(recordPath) - 1] = '\0';
    recordOn = 1;

    if (!rec_cleanup_registered) {
        atexit(rec_cleanup);
        rec_cleanup_registered = true;
    }

    rec_unlock_mp4();
}

void record_stop(void) {
    RecBarrier *barrier;
    RecMsg *stop_msg;
    struct timespec deadline;

    rec_lock_mp4();

    if (!recordOn) {
        rec_unlock_mp4();
        return;
    }

    recordOn = 0;
    recordStartTime = 0;

    barrier = rec_barrier_alloc();
    stop_msg = barrier ? rec_msg_stop(barrier) : NULL;
    if (!stop_msg || !rec_enqueue(stop_msg)) {
        rec_msg_free(stop_msg);
        /* Enqueue failed: the writer never received the barrier, so no other
           thread references it -- free it directly (no refcount dance). */
        if (barrier) rec_barrier_destroy(barrier);
        rec_unlock_mp4();
        HAL_DANGER("record", "Failed to enqueue recording stop message\n");
        return;
    }

    rec_unlock_mp4();

    /* Bounded wait: if the writer is wedged on failing media, do not hang the
       caller (the single HTTP server thread / shutdown) forever. */
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += REC_STOP_DRAIN_TIMEOUT_SEC;

    pthread_mutex_lock(&barrier->mtx);
    while (!barrier->done) {
        if (pthread_cond_timedwait(&barrier->cond, &barrier->mtx, &deadline) == ETIMEDOUT) {
            HAL_WARNING("record", "stop drain timed out; writer stuck on slow media\n");
            break;
        }
    }
    pthread_mutex_unlock(&barrier->mtx);

    rec_barrier_release(barrier);   /* waiter ref */
}

void record_flush(void) {
    RecBarrier *barrier;
    RecMsg *barrier_msg;
    bool running;

    pthread_mutex_lock(&rec_q_mtx);
    running = rec_writer_running;
    pthread_mutex_unlock(&rec_q_mtx);
    if (!running) return;   /* no consumer: the barrier would never be signaled */

    barrier = rec_barrier_alloc();
    barrier_msg = barrier ? rec_msg_barrier(barrier) : NULL;
    if (!barrier_msg || !rec_enqueue(barrier_msg)) {
        rec_msg_free(barrier_msg);
        if (barrier) rec_barrier_destroy(barrier);
        return;
    }

    pthread_mutex_lock(&barrier->mtx);
    while (!barrier->done) {
        pthread_cond_wait(&barrier->cond, &barrier->mtx);
    }
    pthread_mutex_unlock(&barrier->mtx);

    rec_barrier_release(barrier);
}

void send_mp4_to_record(hal_vidstream *stream, char isH265) {
    static enum BufError err;

    if (!recordOn) return;

    for (unsigned int i = 0; i < stream->count; ++i) {
        hal_vidpack *pack = &stream->pack[i];
        unsigned int pack_len = pack->length - pack->offset;
        unsigned char *pack_data = pack->data + pack->offset;

        for (int j = 0; j < pack->naluCnt; j++) {
            if ((pack->nalu[j].type == NalUnitType_SPS ||
                pack->nalu[j].type == NalUnitType_SPS_HEVC) &&
                pack->nalu[j].length >= 4 && pack->nalu[j].length <= UINT16_MAX)
                mp4_set_sps((const char *)(pack_data + pack->nalu[j].offset + 4),
                    pack->nalu[j].length - 4, isH265);
            else if ((pack->nalu[j].type == NalUnitType_PPS ||
                pack->nalu[j].type == NalUnitType_PPS_HEVC) &&
                pack->nalu[j].length >= 4 && pack->nalu[j].length <= UINT16_MAX)
                mp4_set_pps((const char *)(pack_data + pack->nalu[j].offset + 4),
                    pack->nalu[j].length - 4, isH265);
            else if (pack->nalu[j].type == NalUnitType_VPS_HEVC &&
                pack->nalu[j].length >= 4 && pack->nalu[j].length <= UINT16_MAX)
                mp4_set_vps((const char *)(pack_data + pack->nalu[j].offset + 4),
                    pack->nalu[j].length - 4);
            else if ((pack->nalu[j].type == NalUnitType_CodedSliceIdr ||
                pack->nalu[j].type == NalUnitType_CodedSliceAux) &&
                pack->nalu[j].length >= 4)
                mp4_set_slice((const char *)(pack_data + pack->nalu[j].offset + 4),
                    pack->nalu[j].length - 4, 1);
            else if (pack->nalu[j].type == NalUnitType_CodedSliceNonIdr &&
                pack->nalu[j].length >= 4)
                mp4_set_slice((const char *)(pack_data + pack->nalu[j].offset + 4),
                    pack->nalu[j].length - 4, 0);
        }

        if (!recordState.header_sent) {
            struct BitBuf header_buf;

            err = mp4_get_header(&header_buf);
            chk_err_continue
            if (!header_buf.offset) continue;

            if (!rec_enqueue(rec_msg_data_concat((const uint8_t *)header_buf.buf, header_buf.offset,
                NULL, 0, true))) {
                continue;
            }

            recordState.sequence_number = 0;
            recordState.base_data_offset = header_buf.offset;
            recordState.base_media_decode_time = 0;
            recordState.header_sent = true;
            recordState.nals_count = 0;
            recordState.default_sample_duration = default_sample_size;
            rec_venc_seg_bytes = header_buf.offset;
            rec_venc_seg_start = time(NULL);
        }

        err = mp4_set_state(&recordState);
        chk_err_continue

        {
            struct BitBuf moof_buf;
            struct BitBuf mdat_buf;

            err = mp4_get_moof(&moof_buf);
            chk_err_continue
            err = mp4_get_mdat(&mdat_buf);
            chk_err_continue

            rec_enqueue(rec_msg_data_concat((const uint8_t *)moof_buf.buf, moof_buf.offset,
                (const uint8_t *)mdat_buf.buf, mdat_buf.offset, false));
            rec_venc_seg_bytes += moof_buf.offset + mdat_buf.offset;
        }

        if (rec_should_rotate(rec_venc_seg_bytes, rec_venc_seg_start)) {
            char next_path[sizeof(recordPath)];

            /* Cut the segment on the venc side: enqueue ROTATE (the writer opens
               the new file), then reset the muxer so the next fragment regenerates
               ftyp/moov with a zeroed timeline behind that ROTATE. */
            if (rec_build_path(time(NULL), next_path, sizeof(next_path)) &&
                rec_enqueue(rec_msg_rotate(next_path))) {
                recordState.header_sent = false;
                recordState.nals_count = 0;
                rec_venc_seg_bytes = 0;
                rec_venc_seg_start = 0;
            }
        }

        (void)pack_len;
    }


}

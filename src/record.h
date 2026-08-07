#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "app_config.h"
#include "fmt/mp4.h"
#include "hal/macros.h"
#include "hal/types.h"

typedef struct RecSink {
    void *(*open)(const char *path);
    ssize_t (*write)(void *handle, const void *data, size_t len);
    int (*sync)(void *handle);
    int (*close)(void *handle);
} RecSink;

extern size_t record_queue_cap_bytes;

void record_start(void);
void record_stop(void);
void send_mp4_to_record(hal_vidstream *stream, char isH265);
void record_set_sink(const RecSink *sink);
void record_set_queue_cap(size_t cap_bytes);
/* Block until the writer thread has drained all queued recording data
   (segment rotation observable). Off the hot path; used at teardown / by tests. */
void record_flush(void);

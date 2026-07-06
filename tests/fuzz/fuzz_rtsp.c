/* libFuzzer harness for the RTSP control-channel request parser.
 *
 * Drives the internal parser entrypoint __message_proc_sock (src/rtsp/rtsp.c)
 * with a fuzzed request over in-memory streams, mirroring tests/unit/
 * test_rtsp_parse.c. Build with clang -fsanitize=fuzzer,address,undefined.
 *
 *   clang -fsanitize=fuzzer,address,undefined -I<src> \
 *       tests/fuzz/fuzz_rtsp.c src/hal/tools.c src/hal/captime.c \
 *       src/http_headers.c -pthread -o fuzz_rtsp
 *   ./fuzz_rtsp -runs=1000000 -max_len=4096 tests/fuzz/corpus_rtsp
 */
#define _GNU_SOURCE

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

/* Compile the parser directly to reach the static entrypoint. */
#include "rtsp/rtsp.c"

/* rtsp.c calls request_idr() on PLAY; no encoder in the harness. */
void request_idr(void) {}

static struct connection_item_t *g_pool_con;

static void *pool_get_con(int index) {
    (void)index;
    return g_pool_con;
}

static struct connection_item_t *new_connection(void) {
    struct connection_item_t *con = calloc(1, sizeof(*con));
    assert(con);

    con->client_fd = 3;
    con->con_state = __CON_S_INIT;
    pthread_mutex_init(&con->write_mutex, NULL);

    g_pool_con = con;
    con->pool = bufpool_create(1, pool_get_con, NULL, sizeof(*con));
    assert(con->pool);
    assert(bufpool_attach(con->pool, con) == SUCCESS);

    return con;
}

static void free_connection(struct connection_item_t *con) {
    if (!con) return;
    if (con->fp_tcp_read) FCLOSE(con->fp_tcp_read);
    if (con->fp_tcp_write) FCLOSE(con->fp_tcp_write);
    bufpool_delete(con->pool);
    pthread_mutex_destroy(&con->write_mutex);
    free(con);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    /* Bound the request so a pathological length doesn't dominate the run. */
    if (len == 0 || len > 64 * 1024) return 0;

    char *req = malloc(len);
    if (!req) return 0;
    memcpy(req, data, len);

    struct __rtsp_obj_t rtsp;
    memset(&rtsp, 0, sizeof(rtsp));
    rtsp.ctx = 1;

    struct connection_item_t *con = new_connection();

    char *resp_buf = NULL;
    size_t resp_len = 0;
    con->fp_tcp_read = fmemopen(req, len, "r");
    con->fp_tcp_write = open_memstream(&resp_buf, &resp_len);

    if (con->fp_tcp_read && con->fp_tcp_write) {
        struct sock_select_t socks;
        memset(&socks, 0, sizeof(socks));
        socks.h_rtsp = &rtsp;
        FD_ZERO(&socks.rfds);
        FD_SET(con->client_fd, &socks.rfds);

        __message_proc_sock(&con->list_entry, &socks);
        fflush(con->fp_tcp_write);
    }

    free_connection(con);   /* closes the streams (finalizes resp_buf) */
    free(resp_buf);
    free(req);
    return 0;
}

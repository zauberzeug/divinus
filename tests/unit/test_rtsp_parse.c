#define _GNU_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

/*
 * Host-side parser contract test for src/rtsp/rtsp.c.
 *
 * We include the C file directly to drive the internal parser entrypoint
 * (__message_proc_sock) with deterministic in-memory streams.
 */
#include "rtsp/rtsp.c"

void request_idr(void) {}

static struct connection_item_t *g_pool_con;

static void *pool_get_con(int index) {
    assert(index == 0);
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
    if (!con) {
        return;
    }

    FCLOSE(con->fp_tcp_read);
    FCLOSE(con->fp_tcp_write);

    bufpool_delete(con->pool);
    pthread_mutex_destroy(&con->write_mutex);
    free(con);
}

static char *dispatch_request(
    struct connection_item_t *con,
    struct __rtsp_obj_t *rtsp,
    const char *request
) {
    char *req_buf = strdup(request);
    assert(req_buf);

    char *resp_buf = NULL;
    size_t resp_len = 0;

    con->fp_tcp_read = fmemopen(req_buf, strlen(req_buf), "r");
    assert(con->fp_tcp_read);
    con->fp_tcp_write = open_memstream(&resp_buf, &resp_len);
    assert(con->fp_tcp_write);

    struct sock_select_t socks;
    memset(&socks, 0, sizeof(socks));
    socks.h_rtsp = rtsp;
    FD_ZERO(&socks.rfds);
    FD_SET(con->client_fd, &socks.rfds);

    assert(__message_proc_sock(&con->list_entry, &socks) == SUCCESS);

    fflush(con->fp_tcp_write);
    FCLOSE(con->fp_tcp_read);
    FCLOSE(con->fp_tcp_write);
    free(req_buf);

    if (!resp_buf) {
        resp_buf = strdup("");
        assert(resp_buf);
    }
    return resp_buf;
}

static void require_contains(const char *haystack, const char *needle) {
    assert(strstr(haystack, needle) != NULL);
}

static void case_34_setup_track_oob_contract(void) {
    fprintf(stderr, "[rtsp-parse case34] setup track=9 contract\n");

    struct __rtsp_obj_t rtsp;
    memset(&rtsp, 0, sizeof(rtsp));
    rtsp.ctx = 1;

    struct connection_item_t *con = new_connection();

    char *resp = dispatch_request(
        con,
        &rtsp,
        "SETUP rtsp://x/track=9 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n"
    );

    /*
     * Fixed behavior contract:
     * - no OOB on trans[track_id]
     * - response is not a successful SETUP for invalid track
     */
    require_contains(resp, "RTSP/1.0 ");
    assert(strstr(resp, " 200 OK\r\n") == NULL);
    require_contains(resp, "\r\n\r\n");

    free(resp);
    free_connection(con);
}

static void case_33_method_and_disconnect_contract(void) {
    fprintf(stderr, "[rtsp-parse case33] get_parameter + bogus + empty-line contract\n");

    struct __rtsp_obj_t rtsp;
    memset(&rtsp, 0, sizeof(rtsp));
    rtsp.ctx = 7;

    struct connection_item_t *con = new_connection();

    char *setup_resp = dispatch_request(
        con,
        &rtsp,
        "SETUP rtsp://x/track=0 RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n"
    );
    require_contains(setup_resp, "RTSP/1.0 200 OK\r\n");
    free(setup_resp);

    char *getp_resp = dispatch_request(
        con,
        &rtsp,
        "GET_PARAMETER rtsp://x/stream0 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "\r\n"
    );
    require_contains(getp_resp, "RTSP/1.0 200 OK\r\n");
    require_contains(getp_resp, "CSeq: 2\r\n");
    assert(con->con_state != __CON_S_DISCONNECTED);
    free(getp_resp);

    char *bogus_resp = dispatch_request(
        con,
        &rtsp,
        "FROBNICATE rtsp://x/stream0 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "\r\n"
    );
    require_contains(bogus_resp, "RTSP/1.0 501 ");
    require_contains(bogus_resp, "CSeq: 3\r\n");
    assert(con->con_state != __CON_S_DISCONNECTED);
    free(bogus_resp);

    char *empty_resp = dispatch_request(con, &rtsp, "\r\n");
    (void)empty_resp;
    assert(con->con_state == __CON_S_DISCONNECTED);
    free(empty_resp);

    free_connection(con);
}

static void case_37_pause_complete_response_contract(void) {
    fprintf(stderr, "[rtsp-parse case37] pause full-response contract\n");

    struct __rtsp_obj_t rtsp;
    memset(&rtsp, 0, sizeof(rtsp));
    rtsp.ctx = 11;

    struct connection_item_t *con = new_connection();

    char *resp = dispatch_request(
        con,
        &rtsp,
        "PAUSE rtsp://x/stream0 RTSP/1.0\r\n"
        "CSeq: 9\r\n"
        "\r\n"
    );

    require_contains(resp, "RTSP/1.0 ");
    require_contains(resp, "CSeq: 9\r\n");

    size_t len = strlen(resp);
    assert(len >= 4);
    assert(!strcmp(resp + len - 4, "\r\n\r\n"));

    free(resp);
    free_connection(con);
}

int main(void) {
    const char *which = getenv("RTSP_PARSE_CASE");

    if (!which || !strcmp(which, "all")) {
        case_34_setup_track_oob_contract();
        case_33_method_and_disconnect_contract();
        case_37_pause_complete_response_contract();
        return 0;
    }

    if (!strcmp(which, "34")) {
        case_34_setup_track_oob_contract();
        return 0;
    }
    if (!strcmp(which, "33")) {
        case_33_method_and_disconnect_contract();
        return 0;
    }
    if (!strcmp(which, "37")) {
        case_37_pause_complete_response_contract();
        return 0;
    }

    assert(!"Unknown RTSP_PARSE_CASE value");
    return 0;
}

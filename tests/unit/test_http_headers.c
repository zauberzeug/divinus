/* Behavioral tests for src/http_headers.c: header tokenization and lookup
   as used by the HTTP server (request line consumed first, then headers). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_headers.h"

/* Mimics parse_request(): consume the request line, then hand the strtok_r
   state over to the header tokenizer. Returns the malloc'd buffer. */
static char *parse(http_header_t *headers, const char *request) {
    char *buf = strdup(request);
    char *state = NULL;
    strtok_r(buf, " \t\r\n", &state);
    strtok_r(NULL, " \t", &state);
    strtok_r(NULL, " \t\r\n", &state);
    http_headers_parse(headers, &state, buf + strlen(request));
    return buf;
}

static void test_parse_and_get(void) {
    http_header_t headers[HTTP_MAX_HEADERS + 1] = {0};
    char *buf = parse(headers,
        "GET /image.jpg?qp=20 HTTP/1.1\r\n"
        "Host: 192.168.1.94\r\n"
        "Authorization: Basic dXNlcjpwYXNz\r\n"
        "Accept: */*\r\n"
        "\r\n");

    assert(!strcmp(http_headers_get(headers, "Host"), "192.168.1.94"));
    assert(!strcmp(http_headers_get(headers, "Authorization"),
        "Basic dXNlcjpwYXNz"));
    assert(!strcmp(http_headers_get(headers, "accept"), "*/*"));
    assert(!http_headers_get(headers, "Content-Type"));

    free(buf);
}

/* A request with fewer headers than its predecessor must not leave stale
   entries pointing into the predecessor's freed buffer (finding #56: UAF
   that could leak a prior connection's Authorization value). */
static void test_no_stale_headers_from_previous_request(void) {
    static http_header_t headers[HTTP_MAX_HEADERS + 1];

    char *first = parse(headers,
        "GET /mjpeg HTTP/1.1\r\n"
        "Host: 192.168.1.94\r\n"
        "User-Agent: curl/8.0\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: gzip\r\n"
        "Authorization: Basic dXNlcjpwYXNz\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Referer: http://192.168.1.94/\r\n"
        "Cookie: session=1\r\n"
        "\r\n");
    assert(http_headers_get(headers, "Authorization"));
    free(first);

    char *second = parse(headers,
        "GET / HTTP/1.1\r\n"
        "Host: 192.168.1.94\r\n"
        "Accept: */*\r\n"
        "\r\n");
    assert(!strcmp(http_headers_get(headers, "Host"), "192.168.1.94"));
    assert(!http_headers_get(headers, "Authorization"));
    free(second);
}

/* The expected Basic credential must be bounded regardless of the
   configured user/pass lengths (finding #63: stack smash via strcpy). */
static void test_basic_auth_bounded(void) {
    char valid[HTTP_BASIC_AUTH_MAX];

    http_basic_auth(valid, sizeof(valid), "user", "pass");
    assert(!strcmp(valid, "Basic dXNlcjpwYXNz"));

    char user[32], pass[32];
    memset(user, 'u', sizeof(user) - 1);
    memset(pass, 'p', sizeof(pass) - 1);
    user[sizeof(user) - 1] = pass[sizeof(pass) - 1] = '\0';
    http_basic_auth(valid, sizeof(valid), user, pass);
    assert(!strncmp(valid, "Basic ", 6));
    assert(strlen(valid) == 6 + 4 * ((31 + 1 + 31 + 2) / 3));

    char huge[512];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    http_basic_auth(valid, sizeof(valid), huge, huge);
    assert(strlen(valid) < sizeof(valid));
    assert(!strncmp(valid, "Basic ", 6));
}

int main(void) {
    test_parse_and_get();
    test_no_stale_headers_from_previous_request();
    test_basic_auth_bounded();
    puts("test_http_headers: OK");
    return 0;
}

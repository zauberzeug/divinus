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

int main(void) {
    test_parse_and_get();
    puts("test_http_headers: OK");
    return 0;
}

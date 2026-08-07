/* libFuzzer harness for HTTP request-line + header parsing.
 *
 * Mirrors parse_request() in src/server.c: consume the request line, then run
 * the header tokenizer (src/http_headers.c) and exercise lookups. Build with
 * clang -fsanitize=fuzzer,address,undefined.
 *
 *   clang -fsanitize=fuzzer,address,undefined -I<src> \
 *       tests/fuzz/fuzz_http.c src/http_headers.c -o fuzz_http
 *   ./fuzz_http -runs=1000000 -max_len=4096 tests/fuzz/corpus_http
 */
#define _GNU_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "http_headers.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    if (len == 0 || len > 64 * 1024) return 0;

    char *buf = malloc(len + 1);
    if (!buf) return 0;
    memcpy(buf, data, len);
    buf[len] = '\0';

    http_header_t headers[HTTP_MAX_HEADERS + 1] = {0};
    char *state = NULL;

    /* parse_request(): method, URI, version off the request line, then headers.
       `end` is the original buffer end (strtok_r rewrites separators in place). */
    strtok_r(buf, " \t\r\n", &state);
    strtok_r(NULL, " \t", &state);
    strtok_r(NULL, " \t\r\n", &state);
    http_headers_parse(headers, &state, buf + len);

    /* exercise lookup: present, case-insensitive, and absent names */
    http_headers_get(headers, "Authorization");
    http_headers_get(headers, "host");
    http_headers_get(headers, "Content-Length");

    /* and the Basic-auth formatter on a fuzz-derived credential */
    char auth[HTTP_BASIC_AUTH_MAX];
    char *user = http_headers_get(headers, "X-User");
    char *pass = http_headers_get(headers, "X-Pass");
    http_basic_auth(auth, sizeof(auth), user ? user : "", pass ? pass : "");

    free(buf);
    return 0;
}

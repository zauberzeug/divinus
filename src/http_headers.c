#include "http_headers.h"

#include "hal/tools.h"

/* Tokenizes "Name: value" lines into headers[], continuing the strtok_r
   pass the caller started on the request line. The entries point into the
   caller's request buffer and stay valid only as long as it does. */
void http_headers_parse(http_header_t *headers, char **state, const char *end) {
    http_header_t *h = headers;
    memset(headers, 0, (HTTP_MAX_HEADERS + 1) * sizeof(*headers));
    while (h < headers + HTTP_MAX_HEADERS) {
        char *k, *v, *e;
        if (!(k = strtok_r(NULL, "\r\n: \t", state)))
            break;
        v = strtok_r(NULL, "\r\n", state);
        if (!v) break;
        while (*v && *v == ' ' && v++);
        h->name = k;
        h++->value = v;
#ifdef DEBUG_HTTP
        fprintf(stderr, "         (H) %s: %s\n", k, v);
#endif
        if (*state >= end) break;
        e = v + 1 + strlen(v);
        if (e[1] == '\r' && e[2] == '\n')
            break;
    }
}

char *http_headers_get(const http_header_t *headers, const char *name) {
    for (const http_header_t *h = headers; h->name; h++)
        if (!strcasecmp(h->name, name))
            return h->value;
    return NULL;
}

/* Builds the expected "Basic <base64(user:pass)>" value without ever
   writing past out or the internal credential buffer. */
void http_basic_auth(char *out, size_t out_size, const char *user,
    const char *pass) {
    char cred[HTTP_BASIC_CRED_MAX];
    int len = snprintf(cred, sizeof(cred), "%s:%s", user, pass);
    if (len >= (int)sizeof(cred))
        len = sizeof(cred) - 1;
    if (out_size < 6 + (size_t)base64_encode_length(len)) {
        if (out_size) *out = '\0';
        return;
    }
    memcpy(out, "Basic ", 6);
    base64_encode(out + 6, cred, len);
}

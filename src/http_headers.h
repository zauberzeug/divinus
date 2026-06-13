#pragma once

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HTTP_MAX_HEADERS 16

/* "Basic " + base64 of a user:pass credential (HTTP_BASIC_CRED_MAX) + NUL */
#define HTTP_BASIC_CRED_MAX 128
#define HTTP_BASIC_AUTH_MAX \
    (6 + (HTTP_BASIC_CRED_MAX - 1 + 2) / 3 * 4 + 1)

typedef struct {
    char *name, *value;
} http_header_t;

void http_headers_parse(http_header_t *headers, char **state, const char *end);
char *http_headers_get(const http_header_t *headers, const char *name);
void http_basic_auth(char *out, size_t out_size, const char *user,
    const char *pass);

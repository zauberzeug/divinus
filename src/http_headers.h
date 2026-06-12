#pragma once

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define HTTP_MAX_HEADERS 16

typedef struct {
    char *name, *value;
} http_header_t;

void http_headers_parse(http_header_t *headers, char **state, const char *end);
char *http_headers_get(const http_header_t *headers, const char *name);

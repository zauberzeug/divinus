#include "stream_cfg.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "hal/config.h"
#include "hal/macros.h"
#include "hal/tools.h"

#define STREAM_DEST_SLOTS \
    ((int)(sizeof(((struct AppConfig *)0)->stream_dests) / STREAM_DEST_MAX))

int stream_parse_dest(const char *dest, char *host_out, size_t host_sz,
                      unsigned short *port_out, int *is_mcast) {
    if (!dest || !host_out || host_sz < INET_ADDRSTRLEN) return -1;
    if (strlen(dest) >= (size_t)STREAM_DEST_MAX) return -1;
    if (!STARTS_WITH(dest, "udp://")) return -1;

    const char *host = dest + 6;
    const char *colon = strrchr(host, ':');
    if (!colon || colon == host) return -1;

    size_t hostlen = colon - host;
    if (hostlen >= host_sz) return -1;
    memcpy(host_out, host, hostlen);
    host_out[hostlen] = '\0';

    struct in_addr ipv4;
    if (inet_pton(AF_INET, host_out, &ipv4) != 1) return -1;

    const char *p = colon + 1;
    if (!*p) return -1;
    unsigned long port = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        port = port * 10 + (unsigned)(*p - '0');
        if (port > 65535) return -1;
    }
    if (port == 0) return -1;

    if (port_out) *port_out = (unsigned short)port;
    if (is_mcast) {
        unsigned octet = ntohl(ipv4.s_addr) >> 24;
        *is_mcast = (octet >= 224 && octet <= 239);
    }
    return 0;
}

int stream_api_format(char *buf, size_t sz, const struct AppConfig *cfg) {
    int n = snprintf(buf, sz,
        "{\"enable\":%s,\"udp_srcport\":%u,\"dest\":\"%s\",\"dests\":[",
        cfg->stream_enable ? "true" : "false", cfg->stream_udp_srcport,
        cfg->stream_dests[0]);

    for (int i = 0, first = 1; i < STREAM_DEST_SLOTS && *cfg->stream_dests[i]; i++) {
        n += snprintf(buf + n, n < (int)sz ? sz - n : 0, "%s\"%s\"",
            first ? "" : ",", cfg->stream_dests[i]);
        first = 0;
    }
    n += snprintf(buf + n, n < (int)sz ? sz - n : 0, "]}");
    return n;
}

void stream_apply_query(char *query, struct AppConfig *cfg, int *dest_rejected) {
    if (dest_rejected) *dest_rejected = 0;
    while (query) {
        char *value = split(&query, "&");
        if (!value || !*value) continue;
        unescape_uri(value);
        char *key = split(&value, "=");
        if (!key || !*key || !value || !*value) continue;

        if (EQUALS(key, "enable")) {
            if (EQUALS_CASE(value, "true") || EQUALS(value, "1"))
                cfg->stream_enable = true;
            else if (EQUALS_CASE(value, "false") || EQUALS(value, "0"))
                cfg->stream_enable = false;
        } else if (EQUALS(key, "dest")) {
            char host[INET_ADDRSTRLEN];
            unsigned short port;
            if (stream_parse_dest(value, host, sizeof(host), &port, NULL) == 0) {
                strncpy(cfg->stream_dests[0], value,
                    sizeof(cfg->stream_dests[0]) - 1);
                cfg->stream_dests[0][sizeof(cfg->stream_dests[0]) - 1] = '\0';
            } else if (dest_rejected) *dest_rejected = 1;
        } else if (EQUALS(key, "udp_srcport")) {
            char *remain;
            long result = strtol(value, &remain, 10);
            if (remain != value && result >= 0 && result <= 65535)
                cfg->stream_udp_srcport = (unsigned short)result;
        }
    }
}

void stream_config_write(FILE *file, const struct AppConfig *cfg) {
    fprintf(file, "stream:\n");
    fprintf(file, "  enable: %s\n", cfg->stream_enable ? "true" : "false");
    fprintf(file, "  udp_srcport: %d\n", cfg->stream_udp_srcport);
    if (!EMPTY(*cfg->stream_dests)) {
        /* Each item on its own line: parse_list anchors entries to a leading
           newline, so a first dest sharing the `dest:` line would be dropped. */
        fprintf(file, "  dest:\n");
        for (int i = 0; i < STREAM_DEST_SLOTS && *cfg->stream_dests[i]; i++)
            fprintf(file, "    - %s\n", cfg->stream_dests[i]);
    }
}

void stream_config_parse(struct IniConfig *ini, struct AppConfig *cfg) {
    parse_bool(ini, "stream", "enable", &cfg->stream_enable);
    if (!cfg->stream_enable) return;

    int val;
    if (parse_int(ini, "stream", "udp_srcport", 0, USHRT_MAX, &val) == CONFIG_OK)
        cfg->stream_udp_srcport = (unsigned short)val;

    unsigned int count = 0;
    parse_list(ini, "stream", "dest", STREAM_DEST_SLOTS, &count, cfg->stream_dests);
    *cfg->stream_dests[count] = '\0';
}

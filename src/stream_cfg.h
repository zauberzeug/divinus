#pragma once

#include <stddef.h>
#include <stdio.h>

#include "app_config.h"
#include "hal/config.h"

/* Largest destination string that fits a stream_dests[] slot (incl. the NUL). */
#define STREAM_DEST_MAX ((int)sizeof(((struct AppConfig *)0)->stream_dests[0]))

/* Parse a UDP push destination of the form "udp://host:port".
   On success host_out (bounded by host_sz, which must be >= INET_ADDRSTRLEN)
   receives the dotted-quad host, *port_out the port (1..65535) and *is_mcast 1
   when the host is in 224.0.0.0..239.255.255.255. Returns 0 on success, -1 when
   the scheme is wrong, the host is not a valid IPv4 address, the port is
   missing/non-numeric/out of range, or the dest would overflow host_out or a
   stream_dests[] slot. Never writes past host_out[host_sz - 1]. Pure: no globals,
   no sockets — factored out of media_start() so it is host-testable. */
int stream_parse_dest(const char *dest, char *host_out, size_t host_sz,
                      unsigned short *port_out, int *is_mcast);

/* Render the /api/stream JSON body into buf (NUL-terminated, bounded by sz).
   Shape: {"enable":bool,"udp_srcport":N,"dest":"...","dests":[...]} — "dest" is
   the scalar slot-0 mirror the web UI binds to, "dests" the full array for API
   clients. Returns the byte count written (excluding the NUL). */
int stream_api_format(char *buf, size_t sz, const struct AppConfig *cfg);

/* Apply one /api/stream query string ("key=value&...", modified in place by
   split()) to cfg. Recognizes enable, dest and udp_srcport. A malformed dest is
   rejected and leaves cfg->stream_dests untouched; *dest_rejected (when non-NULL)
   is set so the caller can surface it. Mirrors the sibling /api/* POST handlers
   in server.c but stays HAL-free so it links host-side. */
void stream_apply_query(char *query, struct AppConfig *cfg, int *dest_rejected);

/* Write the `stream:` config block for cfg to file. Each destination goes on its
   own line so parse_list (which anchors entries to a leading newline) reads them
   all back. The inverse of stream_config_parse. */
void stream_config_write(FILE *file, const struct AppConfig *cfg);

/* Parse the `stream:` block from ini into cfg (enable, udp_srcport, dests), only
   reading destinations when the stream is enabled — mirrors the sibling section
   parsers in app_config.c. */
void stream_config_parse(struct IniConfig *ini, struct AppConfig *cfg);

#pragma once

#include <stddef.h>
#include <sys/uio.h>

#include "../hal/types.h"

/* Total encoded payload bytes across a stream's packs, for Content-Length. */
size_t multipart_payload_size(const hal_vidstream *stream);

/* Fills iov with one complete part — the caller-formatted prefix, every
   pack's payload straight out of the encoder buffer, and the closing CRLF —
   so the JPEG bytes are framed without being copied. iov must hold
   stream->count + 2 entries. Returns the iovec count. */
int multipart_part_iov(struct iovec *iov, char *prefix, size_t prefix_len,
    const hal_vidstream *stream);

/* Multipart JPEG part assembly: frames a hal_vidstream's packs as
   [prefix, payloads..., CRLF] iovecs pointing into the encoder buffer, so
   the payload is sent zero-copy. Safe because the send completes before the
   HAL frees the stream. */

#include "multipart.h"

size_t multipart_payload_size(const hal_vidstream *stream) {
    size_t size = 0;
    for (unsigned int i = 0; i < stream->count; i++)
        size += stream->pack[i].length - stream->pack[i].offset;
    return size;
}

int multipart_part_iov(struct iovec *iov, char *prefix, size_t prefix_len,
    const hal_vidstream *stream) {
    iov[0].iov_base = prefix;
    iov[0].iov_len = prefix_len;
    for (unsigned int i = 0; i < stream->count; i++) {
        iov[i + 1].iov_base = stream->pack[i].data + stream->pack[i].offset;
        iov[i + 1].iov_len = stream->pack[i].length - stream->pack[i].offset;
    }
    iov[stream->count + 1].iov_base = (void*)"\r\n";
    iov[stream->count + 1].iov_len = 2;
    return stream->count + 2;
}

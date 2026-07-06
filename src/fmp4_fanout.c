#include "fmp4_fanout.h"

#include <stdio.h>

enum SockSendResult fmp4_send_fragment(int fd, struct Mp4State *st) {
    if (mp4_set_state(st) != BUF_OK)
        return SOCK_SEND_SKIPPED;

    struct BitBuf moof_buf, mdat_buf;
    mp4_get_moof(&moof_buf);
    mp4_get_mdat(&mdat_buf);

    char len_buf[50], mdat_len_buf[50];
    ssize_t len_size = sprintf(len_buf, "%zX\r\n", (ssize_t)moof_buf.offset);
    ssize_t mdat_len_size = sprintf(mdat_len_buf, "%zX\r\n", (ssize_t)mdat_buf.offset);
    struct iovec iov[6] = {
        {len_buf, (size_t)len_size},
        {moof_buf.buf, moof_buf.offset},
        {(void*)"\r\n", 2},
        {mdat_len_buf, (size_t)mdat_len_size},
        {mdat_buf.buf, mdat_buf.offset},
        {(void*)"\r\n", 2},
    };

    return sock_send_frame_or_skip(fd, iov, 6);
}

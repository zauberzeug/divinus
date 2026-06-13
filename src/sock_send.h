#pragma once

#include <sys/uio.h>

enum SockSendResult {
    SOCK_SEND_SENT = 0,    /* full frame delivered to the kernel */
    SOCK_SEND_SKIPPED = 1, /* backpressure: zero bytes written, frame dropped */
    SOCK_SEND_DEAD = -1    /* peer is gone or clearly stuck: disconnect it */
};

/* Complete-or-skip transmission of one self-contained frame (e.g. a
   multipart JPEG part) on a non-blocking TCP socket.

   Either the whole frame reaches the kernel send queue or not a single
   byte does, so stream framing can never tear: a backed-up but live peer
   gets SKIPPED (it will pick up again at the next frame boundary), and
   only a dead or clearly-stuck peer gets DEAD.

   The iov array is clobbered (entries are advanced in place); the buffers
   it points to are left untouched. */
enum SockSendResult sock_send_frame_or_skip(int fd, struct iovec *iov, int iovcnt);

/* Complete-or-skip frame transmission for multipart HTTP streams.

   Pure socket logic, no server state: a frame is only started when it fits
   the socket's free send-buffer space, so a backed-up client skips whole
   frames (lower fps, always fresh) instead of being disconnected or fed a
   torn frame, and the producer thread never waits on it. */

#include "sock_send.h"

#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <linux/sockios.h>

/* Committed-send budget: a started frame must finish, so allow the payload
   to drain at a 1 MB/s floor plus margin. Only a clearly-stuck peer (vs. a
   merely slow one, which gets frames skipped before commitment) runs this
   out. */
#define SOCK_SEND_FLOOR_BYTES_PER_MS 1000
#define SOCK_SEND_MARGIN_MS 250

static long elapsed_ms(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 +
        (now.tv_nsec - start->tv_nsec) / 1000000;
}

enum SockSendResult sock_send_frame_or_skip(int fd, struct iovec *iov, int iovcnt) {
    size_t total = 0;
    struct timespec start;

    if (fd < 0)
        return SOCK_SEND_DEAD;
    for (int i = 0; i < iovcnt; i++)
        total += iov[i].iov_len;
    if (!total)
        return SOCK_SEND_SENT;

    /* Probe before committing: only start the frame if it fits the free
       send-buffer space, so a backed-up peer costs a skipped frame instead
       of a torn one. getsockopt reports the kernel-doubled SO_SNDBUF value
       where roughly half is sk_buff bookkeeping, so half of it is a
       conservative usable-payload capacity; SIOCOUTQ is the payload still
       queued. A failed probe (exotic socket) falls through to the
       committed path. */
    int sndbuf = 0, queued = 0;
    socklen_t optlen = sizeof(sndbuf);
    if (!getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &optlen) &&
        !ioctl(fd, SIOCOUTQ, &queued) && sndbuf > 0 && queued >= 0) {
        size_t usable = (size_t)sndbuf / 2;
        size_t free_space = usable > (size_t)queued ? usable - (size_t)queued : 0;
        if (total <= usable) {
            if (total > free_space)
                return SOCK_SEND_SKIPPED;
        } else if (queued) {
            /* A frame bigger than the whole buffer can never pass the fit
               check; instead of starving the client forever, commit at the
               most favorable moment (queue fully drained) and finish under
               the budget below. */
            return SOCK_SEND_SKIPPED;
        }
    }

    long budget_ms = (long)(total / SOCK_SEND_FLOOR_BYTES_PER_MS) +
        SOCK_SEND_MARGIN_MS;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (iovcnt > 0) {
        struct msghdr msg = {.msg_iov = iov, .msg_iovlen = iovcnt};
        ssize_t n = sendmsg(fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            long left = budget_ms - elapsed_ms(&start);
            if (left <= 0)
                return SOCK_SEND_DEAD;
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, (int)left) < 0 && errno != EINTR)
                return SOCK_SEND_DEAD;
            continue;
        }
        if (n <= 0)
            return SOCK_SEND_DEAD;
        while (iovcnt > 0 && n >= (ssize_t)iov->iov_len) {
            n -= (ssize_t)iov->iov_len;
            iov++;
            iovcnt--;
        }
        if (iovcnt > 0) {
            iov->iov_base = (char *)iov->iov_base + n;
            iov->iov_len -= (size_t)n;
        }
    }

    return SOCK_SEND_SENT;
}

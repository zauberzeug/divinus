/* Behavioral tests for sock_send_frame_or_skip(): complete-or-skip frame
   delivery over real loopback TCP sockets with forced-small send buffers
   for deterministic backpressure (SIOCOUTQ works on TCP; AF_UNIX may not). */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/sockios.h>

#include "sock_send.h"

/* Connected loopback TCP pair; sndbuf/rcvbuf of 0 keep kernel defaults.
   rcvbuf is set on the listener before accept so the window is small from
   the SYN onwards. */
static void tcp_pair(int sndbuf, int rcvbuf, int *sender, int *receiver) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    if (rcvbuf > 0)
        assert(!setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)));
    struct sockaddr_in addr = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(lfd, (struct sockaddr *)&addr, sizeof(addr)));
    assert(!listen(lfd, 1));
    socklen_t alen = sizeof(addr);
    assert(!getsockname(lfd, (struct sockaddr *)&addr, &alen));

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(cfd >= 0);
    if (sndbuf > 0)
        assert(!setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)));
    assert(!connect(cfd, (struct sockaddr *)&addr, sizeof(addr)));
    int afd = accept(lfd, NULL, NULL);
    assert(afd >= 0);
    close(lfd);
    *sender = cfd;
    *receiver = afd;
}

/* One multipart-style frame: small header iov + body iov filled with `tag`.
   Appends the expected wire bytes to `expect` when non-NULL. */
static int send_frame(int fd, char tag, size_t body_len,
                      char *expect, size_t *expect_len) {
    static char body[1 << 20];
    char head[64];
    int head_len = snprintf(head, sizeof(head),
        "--b\r\nContent-Length: %zu\r\n\r\n", body_len);
    assert(body_len <= sizeof(body));
    memset(body, tag, body_len);

    struct iovec iov[2] = {
        {.iov_base = head, .iov_len = (size_t)head_len},
        {.iov_base = body, .iov_len = body_len}
    };
    int res = sock_send_frame_or_skip(fd, iov, 2);
    if (res == SOCK_SEND_SENT && expect) {
        memcpy(expect + *expect_len, head, head_len);
        *expect_len += head_len;
        memset(expect + *expect_len, tag, body_len);
        *expect_len += body_len;
    }
    return res;
}

/* Read everything the receiver will ever get: loop until the sender's queue
   is empty and the receiver sees EWOULDBLOCK. */
static size_t drain(int rcv, int snd, char *out, size_t cap) {
    size_t got = 0;
    assert(!fcntl(rcv, F_SETFL, fcntl(rcv, F_GETFL, 0) | O_NONBLOCK));
    for (int idle = 0; idle < 20;) {
        ssize_t n = recv(rcv, out + got, cap - got, 0);
        if (n > 0) {
            got += (size_t)n;
            idle = 0;
            continue;
        }
        assert(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        int queued = 0;
        assert(!ioctl(snd, SIOCOUTQ, &queued));
        if (!queued)
            idle++;
        usleep(1000);
    }
    return got;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* A healthy fast client gets the whole frame in one call and the peer
   receives exactly the bytes that were handed in. */
static void test_fast_client_receives_exact_frame(void) {
    int snd, rcv;
    tcp_pair(256 * 1024, 0, &snd, &rcv);

    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    assert(send_frame(snd, 'A', 100 * 1024, expect, &expect_len) == SOCK_SEND_SENT);

    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  fast client receives exact frame: OK");
}

/* A live but backed-up client is not disconnected and not fed a torn frame:
   the frame is skipped with zero bytes written, so the byte stream the peer
   eventually reads ends exactly on a frame boundary. */
static void test_backed_up_client_skips_whole_frame(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    int res = SOCK_SEND_SENT, sent = 0;
    char tag = 'A';
    /* the receiver never reads, so the pipe fills after a few frames */
    while (sent < 64 &&
           (res = send_frame(snd, tag, 8 * 1024, expect, &expect_len)) ==
               SOCK_SEND_SENT) {
        sent++;
        tag++;
    }
    assert(sent >= 1);  /* healthy frames went through first */
    assert(sent < 64);  /* the pipe did fill up */
    assert(res == SOCK_SEND_SKIPPED);
    /* still backed up: skipped again, client stays alive */
    assert(send_frame(snd, '!', 8 * 1024, NULL, NULL) == SOCK_SEND_SKIPPED);

    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  backed-up client skips whole frame: OK");
}

/* Skipping is per frame, not per client: once the peer drains its backlog,
   the next frame is delivered complete. */
static void test_client_recovers_after_draining(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    char tag = 'A';
    while (send_frame(snd, tag, 8 * 1024, expect, &expect_len) == SOCK_SEND_SENT)
        tag++;

    /* peer catches up */
    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);

    /* and service resumes with a complete fresh frame */
    expect_len = 0;
    assert(send_frame(snd, 'z', 8 * 1024, expect, &expect_len) == SOCK_SEND_SENT);
    got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  client recovers after draining: OK");
}

/* A genuinely dead peer is reported DEAD (so the caller can disconnect it),
   never mistaken for backpressure, and never kills the process via
   SIGPIPE (the sanitizer run would abort on that). */
static void test_dead_peer_is_detected(void) {
    int snd, rcv;
    tcp_pair(64 * 1024, 0, &snd, &rcv);
    close(rcv); /* first send elicits an RST, after which sends fail hard */

    int res = SOCK_SEND_SENT;
    for (int i = 0; i < 10 && res != SOCK_SEND_DEAD; i++) {
        res = send_frame(snd, 'x', 4 * 1024, NULL, NULL);
        assert(res != SOCK_SEND_SKIPPED); /* dead is not "backed up" */
        usleep(10 * 1000);
    }
    assert(res == SOCK_SEND_DEAD);

    close(snd);
    puts("  dead peer is detected: OK");
}

/* The venc producer thread calls this for every client on every frame, so a
   backed-up client must cost microseconds (a probe), not poll timeouts: 50
   skip calls must stay well under one frame interval. */
static void test_skip_never_blocks_producer(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    while (send_frame(snd, 'f', 8 * 1024, NULL, NULL) == SOCK_SEND_SENT)
        ;

    double t0 = now_ms();
    for (int i = 0; i < 50; i++)
        assert(send_frame(snd, 's', 8 * 1024, NULL, NULL) == SOCK_SEND_SKIPPED);
    double spent = now_ms() - t0;
    assert(spent < 50.0);

    close(snd);
    close(rcv);
    printf("  skip never blocks producer (50 skips in %.2f ms): OK\n", spent);
}

struct reader_arg {
    int fd;
    char *out;
    size_t want, got;
};

static void *reader_thread(void *p) {
    struct reader_arg *a = p;
    while (a->got < a->want) {
        ssize_t n = recv(a->fd, a->out + a->got, a->want - a->got, 0);
        if (n <= 0)
            break;
        a->got += (size_t)n;
    }
    return NULL;
}

/* A frame larger than the whole send buffer can never pass the fit check,
   yet an actively-draining client must still receive it (complete) rather
   than being starved forever. */
static void test_oversize_frame_reaches_draining_client(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    size_t body_len = 128 * 1024;
    char head[64];
    int head_len = snprintf(head, sizeof(head),
        "--b\r\nContent-Length: %zu\r\n\r\n", body_len);
    static char body[1 << 20], got[1 << 20];
    memset(body, 'O', body_len);

    struct reader_arg arg = {.fd = rcv, .out = got,
        .want = (size_t)head_len + body_len};
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, reader_thread, &arg));

    struct iovec iov[2] = {
        {.iov_base = head, .iov_len = (size_t)head_len},
        {.iov_base = body, .iov_len = body_len}
    };
    assert(sock_send_frame_or_skip(snd, iov, 2) == SOCK_SEND_SENT);

    assert(!pthread_join(reader, NULL));
    assert(arg.got == (size_t)head_len + body_len);
    assert(!memcmp(got, head, head_len));
    assert(!memcmp(got + head_len, body, body_len));

    close(snd);
    close(rcv);
    puts("  oversize frame reaches draining client: OK");
}

/* Once committed to an unskippable oversize frame, a peer that stops
   draining entirely is declared DEAD — but only after a payload-scaled
   budget (1 MB/s floor + margin), far more generous than the old flat 33 ms
   deadline, and still bounded. */
static void test_stuck_committed_send_dies_after_budget(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    /* 256 KiB >> the whole pipe; the receiver never reads a byte */
    double t0 = now_ms();
    int res = send_frame(snd, 'X', 256 * 1024, NULL, NULL);
    double spent = now_ms() - t0;

    assert(res == SOCK_SEND_DEAD);
    assert(spent > 100.0);   /* clearly not the flat 33 ms kick */
    assert(spent < 2000.0);  /* but still bounded */

    close(snd);
    close(rcv);
    printf("  stuck committed send dies after budget (%.0f ms): OK\n", spent);
}

int main(void) {
    test_fast_client_receives_exact_frame();
    test_backed_up_client_skips_whole_frame();
    test_client_recovers_after_draining();
    test_dead_peer_is_detected();
    test_skip_never_blocks_producer();
    test_oversize_frame_reaches_draining_client();
    test_stuck_committed_send_dies_after_budget();
    puts("test_sock_send: OK");
    return 0;
}

/* Behavioral tests for the multipart JPEG part assembly: a hal_vidstream's
   packs are framed zero-copy as one [prefix, payloads..., CRLF] iovec and
   sent through sock_send_frame_or_skip over real loopback TCP. */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/sockios.h>

#include "fmt/multipart.h"
#include "sock_send.h"

/* Connected loopback TCP pair; sndbuf/rcvbuf of 0 keep kernel defaults. */
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
        if (n == 0)
            break; /* sender closed: nothing more will arrive */
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        int queued = 0;
        assert(!ioctl(snd, SIOCOUTQ, &queued));
        if (!queued)
            idle++;
        usleep(1000);
    }
    return got;
}

/* One pack as the VENC HAL hands it out: payload starts `offset` bytes into
   the buffer, `length` covers offset + payload. */
static hal_vidpack make_pack(unsigned char *buf, unsigned int offset,
                             unsigned int payload_len, int tag) {
    memset(buf, '#', offset);
    memset(buf + offset, tag, payload_len);
    return (hal_vidpack){.data = buf, .offset = offset,
        .length = offset + payload_len};
}

/* The wire carries exactly prefix + payload (skipping the pack's offset
   bytes) + CRLF, with the JPEG read straight from the pack buffer. */
static void test_single_pack_sent_byte_exact(void) {
    int snd, rcv;
    tcp_pair(256 * 1024, 0, &snd, &rcv);

    static unsigned char pack_buf[64 * 1024];
    hal_vidpack pack = make_pack(pack_buf, 16, 40 * 1024, 'J');
    hal_vidstream stream = {.pack = &pack, .count = 1};
    assert(multipart_payload_size(&stream) == 40 * 1024);

    char prefix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix),
        "--b\r\nContent-Length: %zu\r\n\r\n", multipart_payload_size(&stream));

    struct iovec iov[3];
    assert(multipart_part_iov(iov, prefix, prefix_len, &stream) == 3);
    assert(sock_send_frame_or_skip(snd, iov, 3) == SOCK_SEND_SENT);

    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    memcpy(expect, prefix, prefix_len);
    expect_len += prefix_len;
    memset(expect + expect_len, 'J', 40 * 1024);
    expect_len += 40 * 1024;
    memcpy(expect + expect_len, "\r\n", 2);
    expect_len += 2;

    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  single pack sent byte-exact: OK");
}

/* A frame split across several packs still arrives as one contiguous part:
   prefix, then every payload in pack order, then CRLF (this is also the
   first >2-iovec exercise of sock_send_frame_or_skip). */
static void test_multi_pack_sent_byte_exact(void) {
    int snd, rcv;
    tcp_pair(256 * 1024, 0, &snd, &rcv);

    static unsigned char bufs[3][64 * 1024];
    hal_vidpack packs[3] = {
        make_pack(bufs[0], 16, 30 * 1024, 'A'),
        make_pack(bufs[1], 0, 17, 'B'),
        make_pack(bufs[2], 256, 50 * 1024, 'C'),
    };
    hal_vidstream stream = {.pack = packs, .count = 3};
    assert(multipart_payload_size(&stream) == 30 * 1024 + 17 + 50 * 1024);

    char prefix[64];
    int prefix_len = snprintf(prefix, sizeof(prefix),
        "--b\r\nContent-Length: %zu\r\n\r\n", multipart_payload_size(&stream));

    struct iovec iov[5];
    assert(multipart_part_iov(iov, prefix, prefix_len, &stream) == 5);
    assert(sock_send_frame_or_skip(snd, iov, 5) == SOCK_SEND_SENT);

    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    memcpy(expect, prefix, prefix_len);
    expect_len += prefix_len;
    memset(expect + expect_len, 'A', 30 * 1024);
    expect_len += 30 * 1024;
    memset(expect + expect_len, 'B', 17);
    expect_len += 17;
    memset(expect + expect_len, 'C', 50 * 1024);
    expect_len += 50 * 1024;
    memcpy(expect + expect_len, "\r\n", 2);
    expect_len += 2;

    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  multi pack sent byte-exact: OK");
}

/* Complete-or-skip holds for multi-pack frames too: once the pipe is full
   the whole part is skipped (zero bytes, no torn JPEG), so the bytes the
   peer eventually reads end exactly on a part boundary. */
static void test_backed_up_client_skips_whole_multi_pack_frame(void) {
    int snd, rcv;
    tcp_pair(16 * 1024, 16 * 1024, &snd, &rcv);

    static unsigned char bufs[2][8 * 1024];
    static char expect[1 << 20], got[1 << 20];
    size_t expect_len = 0;
    int res = SOCK_SEND_SENT, sent = 0;

    for (int tag = 'A'; tag < 'A' + 64; tag += 2) {
        hal_vidpack packs[2] = {
            make_pack(bufs[0], 32, 4 * 1024, tag),
            make_pack(bufs[1], 0, 4 * 1024, tag + 1),
        };
        hal_vidstream stream = {.pack = packs, .count = 2};

        char prefix[64];
        int prefix_len = snprintf(prefix, sizeof(prefix),
            "--b\r\nContent-Length: %zu\r\n\r\n",
            multipart_payload_size(&stream));

        struct iovec iov[4];
        assert(multipart_part_iov(iov, prefix, prefix_len, &stream) == 4);
        res = sock_send_frame_or_skip(snd, iov, 4);
        if (res != SOCK_SEND_SENT)
            break;
        sent++;
        memcpy(expect + expect_len, prefix, prefix_len);
        expect_len += prefix_len;
        memset(expect + expect_len, tag, 4 * 1024);
        expect_len += 4 * 1024;
        memset(expect + expect_len, tag + 1, 4 * 1024);
        expect_len += 4 * 1024;
        memcpy(expect + expect_len, "\r\n", 2);
        expect_len += 2;
    }
    assert(sent >= 1);  /* healthy frames went through first */
    assert(sent < 32);  /* the pipe did fill up */
    assert(res == SOCK_SEND_SKIPPED);

    size_t got_len = drain(rcv, snd, got, sizeof(got));
    assert(got_len == expect_len);
    assert(!memcmp(got, expect, expect_len));

    close(snd);
    close(rcv);
    puts("  backed-up client skips whole multi-pack frame: OK");
}

int main(void) {
    test_single_pack_sent_byte_exact();
    test_multi_pack_sent_byte_exact();
    test_backed_up_client_skips_whole_multi_pack_frame();
    puts("test_multipart: OK");
    return 0;
}

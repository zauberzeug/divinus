/* Exercises the http_post_connect() addrinfo walk with fabricated chains
   backed by real loopback sockets. The upstream loop connected to the first
   entry only and dereferenced NULL past the end of the list. */

#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_post.h"

/* Stubs for src/http_post.c link dependencies not under test. */
struct AppConfig app_config;
char keepRunning = 0;
int base64_encode(char *encoded, const char *string, int len) { return 0; }
int jpeg_get(short width, short height, char quality, char grayscale,
    hal_jpegdata *jpeg) { return -1; }

static struct sockaddr_in loopback_addr(unsigned short port) {
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sa;
}

/* A bound port with no listen queue refuses connections; the held fd
   keeps the port from being reused while the test runs. */
static int bind_refusing(struct sockaddr_in *sa) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd != -1);
    *sa = loopback_addr(0);
    assert(!bind(fd, (struct sockaddr *)sa, sizeof(*sa)));
    socklen_t len = sizeof(*sa);
    assert(!getsockname(fd, (struct sockaddr *)sa, &len));
    return fd;
}

static int listen_on_loopback(struct sockaddr_in *sa) {
    int fd = bind_refusing(sa);
    assert(!listen(fd, 1));
    return fd;
}

static struct addrinfo make_entry(struct sockaddr_in *sa, struct addrinfo *next) {
    struct addrinfo ai = {0};
    ai.ai_family = AF_INET;
    ai.ai_socktype = SOCK_STREAM;
    ai.ai_protocol = IPPROTO_TCP;
    ai.ai_addr = (struct sockaddr *)sa;
    ai.ai_addrlen = sizeof(*sa);
    ai.ai_next = next;
    return ai;
}

static void test_connects_to_listening_entry(void) {
    struct sockaddr_in sa;
    int listener = listen_on_loopback(&sa);
    struct addrinfo entry = make_entry(&sa, NULL);

    int fd = http_post_connect(&entry);
    assert(fd != -1);

    int peer = accept(listener, NULL, NULL);
    assert(peer != -1);

    close(peer);
    close(fd);
    close(listener);
}

static void test_walks_past_refused_entry(void) {
    struct sockaddr_in dead, live;
    int holder = bind_refusing(&dead);
    int listener = listen_on_loopback(&live);
    struct addrinfo second = make_entry(&live, NULL);
    struct addrinfo first = make_entry(&dead, &second);

    int fd = http_post_connect(&first);
    assert(fd != -1);

    int peer = accept(listener, NULL, NULL);
    assert(peer != -1);

    close(peer);
    close(fd);
    close(listener);
    close(holder);
}

/* Upstream crashed here: with every connect() failing, the loop guard
   `r != NULL || ret != 0` kept iterating past the end of the list and
   dereferenced NULL in `r = r->ai_next`. */
static void test_unreachable_host_returns_failure(void) {
    struct sockaddr_in dead1, dead2;
    int holder1 = bind_refusing(&dead1);
    int holder2 = bind_refusing(&dead2);
    struct addrinfo second = make_entry(&dead2, NULL);
    struct addrinfo first = make_entry(&dead1, &second);

    assert(http_post_connect(&first) == -1);

    close(holder1);
    close(holder2);
}

static void test_empty_list_returns_failure(void) {
    assert(http_post_connect(NULL) == -1);
}

int main(void) {
    test_connects_to_listening_entry();
    test_walks_past_refused_entry();
    test_unreachable_host_returns_failure();
    test_empty_list_returns_failure();
    puts("test_http_post: OK");
    return 0;
}

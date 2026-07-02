/* Host tests for the /api/stream surface: the UDP-dest parser factored out of
   media_start(), the JSON formatter, and the query applier. These link only
   stream_cfg.c + hal/tools.c (no HAL, no sockets), mirroring how
   test_config_bounds isolates the config parser. app_config is defined here
   because app_config.c is not host-linkable. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "stream_cfg.h"

struct AppConfig app_config;

static void test_dest_parser_accepts_unicast(void) {
    char host[64];
    unsigned short port = 0;
    int mcast = -1;
    assert(stream_parse_dest("udp://192.168.1.50:5600", host, sizeof(host),
                             &port, &mcast) == 0);
    assert(!strcmp(host, "192.168.1.50"));
    assert(port == 5600);
    assert(mcast == 0);
}

static void test_dest_parser_flags_multicast(void) {
    char host[64];
    unsigned short port = 0;
    int mcast = -1;
    /* 224..239 in the first octet is the IPv4 multicast range. */
    assert(stream_parse_dest("udp://239.0.0.1:1234", host, sizeof(host),
                             &port, &mcast) == 0);
    assert(!strcmp(host, "239.0.0.1") && port == 1234 && mcast == 1);
    assert(stream_parse_dest("udp://223.255.255.255:1234", host, sizeof(host),
                             &port, &mcast) == 0 && mcast == 0);
    assert(stream_parse_dest("udp://240.0.0.1:1234", host, sizeof(host),
                             &port, &mcast) == 0 && mcast == 0);
}

static void test_dest_parser_rejects_garbage(void) {
    char host[64];
    unsigned short port = 4242;
    int mcast = 0;
    /* Each must fail and leave the caller free to keep its prior state. */
    assert(stream_parse_dest("udp://192.168.1.50", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://192.168.1.50:", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://192.168.1.50:abc", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://192.168.1.50:99999", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://192.168.1.50:0", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://not.an.ip:5600", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("rtmp://192.168.1.50:5600", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("udp://:5600", host, sizeof(host), &port, &mcast) == -1);
    assert(stream_parse_dest("", host, sizeof(host), &port, &mcast) == -1);
    assert(port == 4242 && mcast == 0); /* outputs untouched on every failure */
}

static void test_dest_parser_rejects_overlong(void) {
    char host[64];
    unsigned short port = 0;
    /* A host longer than the destination buffer must be rejected, not truncated
       into a wrong address or overflowed past host_out. */
    char dest[STREAM_DEST_MAX + 64];
    int n = snprintf(dest, sizeof(dest), "udp://");
    memset(dest + n, '9', sizeof(dest) - n - 6);
    strcpy(dest + sizeof(dest) - 6, ":5600");
    assert(stream_parse_dest(dest, host, sizeof(host), &port, NULL) == -1);

    /* A valid IP whose dest string overruns a stream_dests[] slot via a padded
       form is rejected before it can be stored. */
    char small[8];
    assert(stream_parse_dest("udp://192.168.1.50:5600", small, sizeof(small),
                             &port, NULL) == -1);
}

static void test_api_format_emits_state(void) {
    char buf[1024];
    memset(&app_config, 0, sizeof(app_config));
    app_config.stream_enable = true;
    app_config.stream_udp_srcport = 5600;
    strcpy(app_config.stream_dests[0], "udp://192.168.1.50:5600");

    int n = stream_api_format(buf, sizeof(buf), &app_config);
    assert(n > 0 && (size_t)n == strlen(buf));
    /* DoD shape: {enable, udp_srcport, dests[]} (+ scalar dest mirror for UI). */
    assert(strstr(buf, "\"enable\":true"));
    assert(strstr(buf, "\"udp_srcport\":5600"));
    assert(strstr(buf, "\"dest\":\"udp://192.168.1.50:5600\""));
    assert(strstr(buf, "\"dests\":[\"udp://192.168.1.50:5600\"]"));
}

static void test_api_format_disabled_empty(void) {
    char buf[1024];
    memset(&app_config, 0, sizeof(app_config));
    stream_api_format(buf, sizeof(buf), &app_config);
    assert(strstr(buf, "\"enable\":false"));
    assert(strstr(buf, "\"dest\":\"\""));
    assert(strstr(buf, "\"dests\":[]"));
}

/* Apply mutates a writable copy of the query in place (split()). */
static void apply(const char *query, int *rejected) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", query);
    stream_apply_query(buf, &app_config, rejected);
}

static void test_api_apply_mutates_config(void) {
    memset(&app_config, 0, sizeof(app_config));
    int rejected = -1;
    apply("enable=1&dest=udp://192.168.1.50:5600&udp_srcport=5600", &rejected);
    assert(app_config.stream_enable == true);
    assert(!strcmp(app_config.stream_dests[0], "udp://192.168.1.50:5600"));
    assert(app_config.stream_udp_srcport == 5600);
    assert(rejected == 0);

    /* enable accepts the same truthy/falsey spellings as the sibling handlers. */
    apply("enable=false", NULL);
    assert(app_config.stream_enable == false);
    apply("enable=true", NULL);
    assert(app_config.stream_enable == true);
    apply("enable=0", NULL);
    assert(app_config.stream_enable == false);
}

static void test_api_apply_rejects_bad_dest_without_corrupting(void) {
    memset(&app_config, 0, sizeof(app_config));
    strcpy(app_config.stream_dests[0], "udp://10.0.0.1:5000");
    int rejected = 0;
    apply("dest=udp://garbage:port", &rejected);
    assert(rejected == 1);
    /* The prior destination survives a rejected update verbatim. */
    assert(!strcmp(app_config.stream_dests[0], "udp://10.0.0.1:5000"));

    /* An over-long dest can never overflow the slot. */
    char query[STREAM_DEST_MAX + 64];
    int n = snprintf(query, sizeof(query), "dest=udp://");
    memset(query + n, '9', sizeof(query) - n - 6);
    strcpy(query + sizeof(query) - 6, ":5600");
    rejected = 0;
    stream_apply_query(query, &app_config, &rejected);
    assert(rejected == 1);
    assert(!strcmp(app_config.stream_dests[0], "udp://10.0.0.1:5000"));
}

int main(void) {
    test_dest_parser_accepts_unicast();
    test_dest_parser_flags_multicast();
    test_dest_parser_rejects_garbage();
    test_dest_parser_rejects_overlong();
    test_api_format_emits_state();
    test_api_format_disabled_empty();
    test_api_apply_mutates_config();
    test_api_apply_rejects_bad_dest_without_corrupting();
    puts("test_api_stream: OK");
    return 0;
}

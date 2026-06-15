/* Behavioral tests for the bounded config string parser used to keep
   fixed-size fields like web_auth_user[32] from being overflowed by an
   oversize value in divinus.yaml. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/config.h"
#include "stream_cfg.h"

static struct IniConfig ini;

static void load(const char *cfg) {
    free(ini.str);
    ini.str = strdup(cfg);
    assert(find_sections(&ini) == CONFIG_OK);
}

static void test_value_within_bounds(void) {
    char user[32];
    load("system:\n  web_auth_user: 1234567890123456789012345678901\n");
    assert(parse_param_value_n(&ini, "system", "web_auth_user",
        user, sizeof(user)) == CONFIG_OK);
    assert(strlen(user) == 31);
}

static void test_oversize_value_rejected(void) {
    char user[32];
    memset(user, 'q', sizeof(user) - 1);
    user[sizeof(user) - 1] = '\0';
    load("system:\n  web_auth_user: 12345678901234567890123456789012\n");
    assert(parse_param_value_n(&ini, "system", "web_auth_user",
        user, sizeof(user)) == CONFIG_PARAM_TOO_LONG);
    assert(user[0] == 'q'); /* destination untouched */
}

static void test_quotes_and_whitespace_stripped(void) {
    char user[32];
    load("system:\n  web_auth_user: \"admin\"\n");
    assert(parse_param_value_n(&ini, "system", "web_auth_user",
        user, sizeof(user)) == CONFIG_OK);
    assert(!strcmp(user, "admin"));

    load("system:\n  web_auth_user: admin \n");
    assert(parse_param_value_n(&ini, "system", "web_auth_user",
        user, sizeof(user)) == CONFIG_OK);
    assert(!strcmp(user, "admin"));
}

/* Write the stream block (between sibling sections, the realistic position) and
   re-parse it, asserting the round-trip preserves every field. */
static void roundtrip(const struct AppConfig *in, struct AppConfig *out) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    assert(f);
    fputs("system:\n  web_port: 80\n", f);
    stream_config_write(f, in);
    fputs("audio:\n  enable: false\n", f);
    fclose(f);

    memset(out, 0, sizeof(*out));
    struct IniConfig rt = {0};
    rt.str = buf;
    assert(find_sections(&rt) == CONFIG_OK);
    stream_config_parse(&rt, out);
    free(rt.str);
}

static void test_stream_block_roundtrips_all_dests(void) {
    struct AppConfig in;
    memset(&in, 0, sizeof(in));
    in.stream_enable = true;
    in.stream_udp_srcport = 5600;
    strcpy(in.stream_dests[0], "udp://192.168.1.50:5600");
    strcpy(in.stream_dests[1], "udp://239.0.0.1:1234");    /* multicast form */
    strcpy(in.stream_dests[2], "rtmp://example.com/live"); /* mixed scheme */
    strcpy(in.stream_dests[3], "udp://10.0.0.9:5004");

    struct AppConfig out;
    roundtrip(&in, &out);
    assert(out.stream_enable == true);
    assert(out.stream_udp_srcport == 5600);
    /* All four survive, in order — the first dest used to be written on the
       `dest:` line itself and dropped by the newline-anchored list parser. */
    for (int i = 0; i < 4; i++)
        assert(!strcmp(out.stream_dests[i], in.stream_dests[i]));
}

static void test_stream_block_empty_dest_roundtrips(void) {
    struct AppConfig in;
    memset(&in, 0, sizeof(in));
    in.stream_enable = true;
    in.stream_udp_srcport = 1234;

    struct AppConfig out;
    roundtrip(&in, &out);
    assert(out.stream_enable == true);
    assert(out.stream_udp_srcport == 1234);
    assert(out.stream_dests[0][0] == '\0'); /* no dest key, empty list */
}

int main(void) {
    test_value_within_bounds();
    test_oversize_value_rejected();
    test_quotes_and_whitespace_stripped();
    test_stream_block_roundtrips_all_dests();
    test_stream_block_empty_dest_roundtrips();
    free(ini.str);
    puts("test_config_bounds: OK");
    return 0;
}

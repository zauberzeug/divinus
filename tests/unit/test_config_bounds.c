/* Behavioral tests for the bounded config string parser used to keep
   fixed-size fields like web_auth_user[32] from being overflowed by an
   oversize value in divinus.yaml. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/config.h"

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

int main(void) {
    test_value_within_bounds();
    test_oversize_value_rejected();
    test_quotes_and_whitespace_stripped();
    free(ini.str);
    puts("test_config_bounds: OK");
    return 0;
}

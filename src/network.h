#pragma once

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/utsname.h>

#include "app_config.h"
#include "hal/support.h"
#include "lib/tinysvcmdns.h"
#include "onvif.h"

typedef struct {
    char intf[3][16];
    char ipaddr[3][INET_ADDRSTRLEN];
    char count;
    char host[65];
} NetInfo;

void network_init(void);
int network_start(void);
void network_stop(void);

bool ntp_server_valid(const char *server);
int ntp_apply(void);

int mdns_start(void);
void mdns_stop(void);

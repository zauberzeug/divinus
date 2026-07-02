#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "hal/support.h"
#include "hal/tools.h"
#include "fmt/rtppkt.h"

#define MAX_UDP_PACKET_SIZE 1400
#define UDP_DEFAULT_PORT 5600
#define RTP_HEADER_SIZE 12
/* Largest RTP payload that keeps the whole datagram within MAX_UDP_PACKET_SIZE. */
#define UDP_MAX_PAYLOAD (MAX_UDP_PACKET_SIZE - RTP_HEADER_SIZE)
#define UDP_MAX_CLIENTS 8

typedef struct {
    struct sockaddr_in addr;
    int active;
    unsigned int ssrc;
    unsigned short seq;
    unsigned int tstamp;
    time_t last_act;
} udp_client_t;

struct udp_stream_ctx {
    int socket_fd;
    unsigned short port;
    volatile int running;
    pthread_t thread;
    pthread_mutex_t mutex;
    udp_client_t clients[UDP_MAX_CLIENTS];
    int client_count;
    char is_mcast;
    /* 0 when the next udp_stream_send_nal begins a new access unit: the per-AU
       RTP timestamp is set once at frame start (from the PTS), so every NAL of
       the frame shares it. Set on the first NAL, cleared after end_of_frame. */
    char frame_in_progress;
    unsigned int mcast_addr;
};

int udp_stream_init(unsigned short port, const char *mcast_addr);
void udp_stream_close(void);
int udp_stream_add_client(const char *host, unsigned short port);
int udp_stream_set_client(const char *host, unsigned short port);
void udp_stream_remove_client(int client_id);
int udp_stream_send_nal(const char *nal_data, int nal_size, int end_of_frame, int is_h265,
    unsigned long long pts_us);

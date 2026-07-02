#include "stream.h"
#include "hal/captime.h"

static struct udp_stream_ctx *g_udp_ctx = NULL;

static void *udp_client_manager_thread(void *data);
static int add_rtp_header(unsigned char *packet, int pay_size,
    unsigned short seq, unsigned int tstamp,
    unsigned int ssrc, int marker, int pay_type);

struct udp_emit_ctx {
    int payload_type;
    int throttle_us;   /* spacing between FU fragments; 0 disables */
};
static int udp_emit(void *vctx, const unsigned char *hdr, int hdr_len,
    const unsigned char *body, int body_len, int marker);

/**
 * Initializes the UDP streaming module
 * @param port UDP port to be used (0 = prefer the default value)
 * @param mcast_addr Multicast address to be used (NULL = disabled)
 * @return EXIT_SUCCESS (0) or EXIT_FAILURE (-1)
 */
int udp_stream_init(unsigned short port, const char *mcast_addr) {
    struct sockaddr_in addr;
    int enable = 1;

    /* Idempotent: a second init while already running keeps the live socket,
       manager thread and registered clients rather than orphaning them — the
       caller can re-assert "enabled" without rebinding or leaking. */
    if (g_udp_ctx) return EXIT_SUCCESS;

    /* Build the context fully in a local and publish g_udp_ctx only once it is
       ready, so the encoder thread (save_video_stream -> udp_stream_send_nal,
       which reads g_udp_ctx without the lifecycle lock) never observes a
       half-initialized context during a runtime enable. */
    struct udp_stream_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        HAL_ERROR("stream", "Failed to allocate UDP stream context!\n");

    ctx->port = port ? port : UDP_DEFAULT_PORT;

    if (pthread_mutex_init(&ctx->mutex, NULL)) {
        free(ctx);
        HAL_ERROR("stream", "Failed to initialize mutex!\n");
    }

    /* Own the socket in a local fd; hand it to ctx only once bound, so every
       error path closes exactly one fd and the success path transfers it. */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        HAL_DANGER("stream", "Failed to create UDP socket: %s\n", strerror(errno));
        goto error;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        HAL_DANGER("stream", "Failed to set socket options: %s\n", strerror(errno));
        goto error;
    }

    if (mcast_addr) {
        ctx->is_mcast = 1;
        ctx->mcast_addr = inet_addr(mcast_addr);

        int ttl = 32;
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
            HAL_DANGER("stream", "Failed to set multicast TTL: %s\n", strerror(errno));
    }

    memset(&addr, 0, sizeof(addr));
    addr = (struct sockaddr_in){.sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY, .sin_port = htons(ctx->port)};

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        HAL_DANGER("stream", "Failed to bind UDP socket: %s\n", strerror(errno));
        goto error;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    ctx->socket_fd = fd;
    ctx->running = 1;
    if (pthread_create(&ctx->thread, NULL, udp_client_manager_thread, ctx) != 0) {
        HAL_DANGER("stream", "Failed to create UDP client manager thread!\n");
        goto error;
    }

    g_udp_ctx = ctx;

    HAL_INFO("stream", "UDP streaming initialized on port %d\n", ctx->port);
    if (ctx->is_mcast) {
        char ip_str[INET_ADDRSTRLEN];
        struct in_addr maddr = {.s_addr = ctx->mcast_addr};
        inet_ntop(AF_INET, &maddr, ip_str, INET_ADDRSTRLEN);
        HAL_INFO("stream", "UDP multicast streaming to %s\n", ip_str);
    }

    return EXIT_SUCCESS;

error:
    if (fd >= 0) close(fd);
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx);
    return EXIT_FAILURE;
}

/**
 * Closes and cleans up the UDP streaming module
 */
void udp_stream_close() {
    if (!g_udp_ctx) return;

    g_udp_ctx->running = 0;
    pthread_join(g_udp_ctx->thread, NULL);

    close(g_udp_ctx->socket_fd);

    pthread_mutex_destroy(&g_udp_ctx->mutex);

    free(g_udp_ctx);
    g_udp_ctx = NULL;

    HAL_INFO("stream", "UDP streaming closed\n");
}

/**
 * Adds a new UDP client
 * @param host Client hostname or IP address
 * @param port Client port
 * @return Client ID or -1 on error
 */
int udp_stream_add_client(const char *host, unsigned short port) {
    if (!g_udp_ctx) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        HAL_DANGER("stream", "Invalid address: %s\n", host);
        return -1;
    }

    pthread_mutex_lock(&g_udp_ctx->mutex);

    for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
        if (!g_udp_ctx->clients[i].active) continue;

        if (g_udp_ctx->clients[i].addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
            g_udp_ctx->clients[i].addr.sin_port == addr.sin_port) {
            g_udp_ctx->clients[i].last_act = time(NULL);
            pthread_mutex_unlock(&g_udp_ctx->mutex);
            return i;
        }
    }

    for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
        if (g_udp_ctx->clients[i].active) continue;

        g_udp_ctx->clients[i] = (udp_client_t){
            .addr = addr,
            .active = 1,
            .ssrc = rand(),
            .seq = rand() & 0xFFFF,
            .tstamp = (millis() * 90) & UINT32_MAX,
            .last_act = time(NULL)
        };

        g_udp_ctx->client_count++;

        HAL_INFO("stream", "Added UDP client %s:%d (ID %d)\n",
            host, port, i);

        pthread_mutex_unlock(&g_udp_ctx->mutex);
        return i;
    }

    HAL_DANGER("stream", "Maximum number of UDP clients reached!\n");
    pthread_mutex_unlock(&g_udp_ctx->mutex);
    return -1;
}

/**
 * Makes host:port the sole UDP push destination, replacing any current clients.
 * Used by the runtime /api/stream path to (re)point the push at the configured
 * destination without tearing the context down. Touches only the client table
 * under the mutex — never frees the context — so it is safe against the encoder
 * thread in udp_stream_send_nal().
 * @return 0 on success, -1 on a bad address or before init
 */
int udp_stream_set_client(const char *host, unsigned short port) {
    if (!g_udp_ctx) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        HAL_DANGER("stream", "Invalid address: %s\n", host);
        return -1;
    }

    pthread_mutex_lock(&g_udp_ctx->mutex);
    for (int i = 0; i < UDP_MAX_CLIENTS; i++)
        g_udp_ctx->clients[i].active = 0;
    g_udp_ctx->clients[0] = (udp_client_t){
        .addr = addr,
        .active = 1,
        .ssrc = rand(),
        .seq = rand() & 0xFFFF,
        .tstamp = (millis() * 90) & UINT32_MAX,
        .last_act = time(NULL)
    };
    g_udp_ctx->client_count = 1;
    pthread_mutex_unlock(&g_udp_ctx->mutex);

    HAL_INFO("stream", "UDP push destination set to %s:%d\n", host, port);
    return 0;
}

/**
 * Deletes a UDP client
 * @param client_id Client ID to be removed
 */
void udp_stream_remove_client(int client_id) {
    if (!g_udp_ctx) return;
    if (client_id < 0 || client_id >= UDP_MAX_CLIENTS) return;

    pthread_mutex_lock(&g_udp_ctx->mutex);

    if (g_udp_ctx->clients[client_id].active) {
        char ip_str[INET_ADDRSTRLEN];
        struct sockaddr_in *addr = &g_udp_ctx->clients[client_id].addr;
        uint16_t port = ntohs(addr->sin_port);

        inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);
        g_udp_ctx->clients[client_id].active = 0;
        g_udp_ctx->client_count--;

        HAL_INFO("stream", "Removed UDP client %s:%d (ID %d)\n",
            ip_str, port, client_id);
    }

    pthread_mutex_unlock(&g_udp_ctx->mutex);
}

/**
 * Emit one RTP packet (FU/payload headers + body) to every active client, or to
 * the multicast group. Stamps each client's own sequence/timestamp/SSRC; the
 * shared packetizer supplies the marker. Runs under g_udp_ctx->mutex.
 */
static int udp_emit(void *vctx, const unsigned char *hdr, int hdr_len,
    const unsigned char *body, int body_len, int marker) {
    struct udp_emit_ctx *e = vctx;
    static unsigned char packet[MAX_UDP_PACKET_SIZE + RTP_HEADER_SIZE];
    int payload_len = hdr_len + body_len;

    if (RTP_HEADER_SIZE + payload_len > (int)sizeof(packet)) return EXIT_FAILURE;
    if (hdr_len) memcpy(packet + RTP_HEADER_SIZE, hdr, hdr_len);
    memcpy(packet + RTP_HEADER_SIZE + hdr_len, body, body_len);
    int packet_size = RTP_HEADER_SIZE + payload_len;

    if (g_udp_ctx->is_mcast) {
        struct sockaddr_in mcast_addr;
        memset(&mcast_addr, 0, sizeof(mcast_addr));
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_addr.s_addr = g_udp_ctx->mcast_addr;
        mcast_addr.sin_port = htons(g_udp_ctx->port);

        add_rtp_header(packet, payload_len, 0, 0, 0, marker, e->payload_type);
        sendto(g_udp_ctx->socket_fd, packet, packet_size, 0,
            (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
    } else {
        for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
            if (!g_udp_ctx->clients[i].active) continue;

            add_rtp_header(packet, payload_len, g_udp_ctx->clients[i].seq++,
                g_udp_ctx->clients[i].tstamp, g_udp_ctx->clients[i].ssrc,
                marker, e->payload_type);
            sendto(g_udp_ctx->socket_fd, packet, packet_size, 0,
                (struct sockaddr *)&g_udp_ctx->clients[i].addr,
                sizeof(struct sockaddr_in));
        }
    }

    /* Space out fragments (hdr_len > 0) so a burst of FU packets doesn't
       overrun the socket buffer. Tracked for removal under the latency epic. */
    if (hdr_len && e->throttle_us) usleep(e->throttle_us);
    return EXIT_SUCCESS;
}

/**
 * Send a RTP-encapsulated access-unit pack to all clients
 * @param nal_data Annex-B pack (one or more start-code-prefixed NALs)
 * @param nal_size Size of the pack
 * @param end_of_frame Indicates if this pack ends the access unit
 * @param is_h265 Indicates if the NAL units are H.265
 * @return EXIT_SUCCESS or EXIT_FAILURE
 */
int udp_stream_send_nal(const char *nal_data, int nal_size,
    int end_of_frame, int is_h265, unsigned long long pts_us) {
    if (!g_udp_ctx || !nal_data || nal_size <= 0) return EXIT_FAILURE;

    int total_clients;
    pthread_mutex_lock(&g_udp_ctx->mutex);
    total_clients = g_udp_ctx->client_count;
    pthread_mutex_unlock(&g_udp_ctx->mutex);

    if (total_clients == 0 && !g_udp_ctx->is_mcast) return EXIT_SUCCESS;

    struct udp_emit_ctx emit_ctx = {.payload_type = 96, .throttle_us = 100};
    unsigned char *buf = (unsigned char *)nal_data;

    pthread_mutex_lock(&g_udp_ctx->mutex);

    if (!g_udp_ctx->frame_in_progress) {
        /* Frame start: set this access unit's RTP timestamp from the vendor PTS
           (the monotonic media clock), via the pts_to_rtp90 mapping shared with
           the RTSP sender (rtp.c). 0 falls back to send-time millis(). The
           shared rtp_ts_advance guard keeps the 32-bit stamp strictly
           increasing across the rare duplicate PTS; the per-AU advance happens
           once here so every NAL of the frame carries the same timestamp. */
        unsigned int src90 = pts_us ? pts_to_rtp90(pts_us)
                                    : ((millis() * 90) & UINT32_MAX);
        for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
            if (!g_udp_ctx->clients[i].active) continue;
            g_udp_ctx->clients[i].tstamp =
                rtp_ts_advance(g_udp_ctx->clients[i].tstamp, src90);
        }
        g_udp_ctx->frame_in_progress = 1;
    }

    if (nal_size >= 4 && !buf[0] && !buf[1] && !buf[2] && buf[3] == 1) {
        /* The encoder hands a keyframe access unit as one glued pack
           (VPS+SPS+PPS+IDR). Split it into individual NALs and packetize each
           on its own; the marker rides only the last NAL of the frame. */
        unsigned char *it = buf, *cur;
        size_t it_len = 0, cur_len;
        int have = nal_next(buf, &it, &it_len, nal_size) == 0;
        while (have) {
            cur = it;
            cur_len = it_len;
            int more = nal_next(buf, &it, &it_len, nal_size) == 0;
            rtp_packetize_nal(cur, (int)cur_len, is_h265, UDP_MAX_PAYLOAD,
                end_of_frame && !more, udp_emit, &emit_ctx);
            have = more;
        }
    } else {
        rtp_packetize_nal(buf, nal_size, is_h265, UDP_MAX_PAYLOAD,
            end_of_frame, udp_emit, &emit_ctx);
    }
    if (end_of_frame) g_udp_ctx->frame_in_progress = 0;
    pthread_mutex_unlock(&g_udp_ctx->mutex);

    return EXIT_SUCCESS;
}

/**
 * Thread handler for managing UDP clients (inactivity check)
 */
static void *udp_client_manager_thread(void *data) {
    struct udp_stream_ctx *ctx = (struct udp_stream_ctx *)data;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[1024];
    int recv_len;
    time_t now;

    while (ctx->running) {
        now = time(NULL);

        pthread_mutex_lock(&ctx->mutex);
        for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
            if (ctx->clients[i].active) {
                if (difftime(now, ctx->clients[i].last_act) > 60) {
                    ctx->clients[i].active = 0;
                    ctx->client_count--;

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ctx->clients[i].addr.sin_addr,
                             ip_str, INET_ADDRSTRLEN);
                    HAL_INFO("stream", "Removed inactive UDP client %s:%d (ID %d)\n",
                           ip_str, ntohs(ctx->clients[i].addr.sin_port), i);
                }
            }
        }
        pthread_mutex_unlock(&ctx->mutex);

        recv_len = recvfrom(ctx->socket_fd, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&client_addr, &addr_len);

        if (recv_len > 0) {
            pthread_mutex_lock(&ctx->mutex);

            int client_found = 0;
            for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
                if (!ctx->clients[i].active) continue;

                if (ctx->clients[i].addr.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                    ctx->clients[i].addr.sin_port == client_addr.sin_port) {
                    ctx->clients[i].last_act = now;
                    client_found = 1;
                    break;
                }
            }

            if (!client_found && ctx->client_count < UDP_MAX_CLIENTS) {
                for (int i = 0; i < UDP_MAX_CLIENTS; i++) {
                    if (ctx->clients[i].active) continue;

                    ctx->clients[i].addr = client_addr;
                    ctx->clients[i].active = 1;
                    ctx->clients[i].ssrc = rand();
                    ctx->clients[i].seq = rand() & 0xFFFF;
                    ctx->clients[i].tstamp = (millis() * 90) & UINT32_MAX;
                    ctx->clients[i].last_act = now;

                    ctx->client_count++;

                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
                    HAL_INFO("stream", "Auto-added UDP client %s:%d (ID %d)\n",
                        ip_str, ntohs(client_addr.sin_port), i);
                    break;
                }
            }

            pthread_mutex_unlock(&ctx->mutex);
        }

        usleep(500000);
    }

    return NULL;
}

/**
 * Prefixes the RTP header to a given packet
 */
static int add_rtp_header(unsigned char *packet, int pay_size,
                         unsigned short seq, unsigned int tstamp,
                         unsigned int ssrc, int marker, int pay_type) {
    if (!packet || pay_size <= 0) return 0;

    // RTP header (12 bytes)
    packet[0] = 0x80;  // Version=2, Padding=0, Extension=0, CSRC count=0
    packet[1] = (marker ? 0x80 : 0x00) | (pay_type & 0x7F);

    // Sequence number (16 bits)
    packet[2] = (seq >> 8) & 0xFF;
    packet[3] = seq & 0xFF;

    // Timestamp (32 bits)
    packet[4] = (tstamp >> 24) & 0xFF;
    packet[5] = (tstamp >> 16) & 0xFF;
    packet[6] = (tstamp >> 8) & 0xFF;
    packet[7] = tstamp & 0xFF;

    // SSRC (32 bits)
    packet[8] = (ssrc >> 24) & 0xFF;
    packet[9] = (ssrc >> 16) & 0xFF;
    packet[10] = (ssrc >> 8) & 0xFF;
    packet[11] = ssrc & 0xFF;

    return RTP_HEADER_SIZE;
}
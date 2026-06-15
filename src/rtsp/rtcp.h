#ifndef _RTSP_RTCP_H
#define _RTSP_RTCP_H

#include <stdlib.h>
#include <stdio.h>
#include "rtp.h"
#include "rfc.h"
#include "rtsp.h"
#include "common.h"
#include "../hal/captime.h"

/******************************************************************************
 *              DECLARATIONS
 ******************************************************************************/

static inline int __rtcp_send_sr(struct connection_item_t *con, int track_id);


/******************************************************************************
 *              INLINE FUNCTIONS
 ******************************************************************************/
static inline int __rtcp_send_sr(struct connection_item_t *con, int track_id)
{
    unsigned int ntp_sec, ntp_frac, rtp_ts;
    int send_bytes;
    struct sockaddr_in to_addr;
    transport_t *t;

    ASSERT(track_id >= 0 &&
        track_id < (int)(sizeof(con->trans) / sizeof(con->trans[0])),
        return FAILURE);
    t = &con->trans[track_id];

    if (t->capture_us) {
        /* Anchor the NTP wall-clock and the RTP timestamp to one capture
           instant, so the receiver maps the RTP timeline onto absolute time.
           Both come from the shared captime helper used to stamp the frames. */
        captime_sr_ts sr = captime_sr_from_capture(t->capture_us);
        ntp_sec = sr.ntp_sec;
        ntp_frac = sr.ntp_frac;
        rtp_ts = sr.rtp_ts;
    } else {
        /* No capture time (relative streaming): pair send-time-now with the
           last RTP timestamp — a coherent snapshot, but not absolute. */
        struct timeval tv;
        ASSERT(gettimeofday(&tv, NULL) == 0, return FAILURE);
        ntp_sec = (unsigned int)tv.tv_sec + 2208988800U;
        ntp_frac = (unsigned int)((((double)tv.tv_usec) / 1e6) * 4294967296.0);
        rtp_ts = t->rtp_timestamp;
    }

    rtcp_t rtcp = { common: {version: 2, length: htons(RTCP_SR_NORB_LENGTH), p:0, count: 0, pt:RTCP_SR},
        r: { sr: { ssrc: htonl(con->ssrc),
            ntp_sec: htonl(ntp_sec),
            ntp_frac: htonl(ntp_frac),
            rtp_ts: htonl(rtp_ts),
            psent: htonl(t->rtcp_packet_cnt),
            osent: htonl(t->rtcp_octet)}}};

    if (t->is_tcp) {
        unsigned char head[4];
        head[0] = '$';
        head[1] = t->channel_rtcp;
        head[2] = 0;
        head[3] = RTCP_SR_NORB_BYTES;

        /* a stalled client must not hold write_mutex forever: the venc
           callback serves every connection through this path */
        int spins = 0;
        pthread_mutex_lock(&con->write_mutex);
        int sent_h = 0;
        while (sent_h < 4) {
            int r = send(con->client_fd, head + sent_h, 4 - sent_h, 0);
            if (r > 0) sent_h += r;
            else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                ++spins <= 10) usleep(1000);
            else { sent_h = -1; break; }
        }
        if (sent_h == 4) {
            int sent_b = 0;
            while (sent_b < (int)RTCP_SR_NORB_BYTES) {
                int r = send(con->client_fd, (char*)&(rtcp) + sent_b,
                    RTCP_SR_NORB_BYTES - sent_b, 0);
                if (r > 0) sent_b += r;
                else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                    ++spins <= 10) usleep(1000);
                else { sent_b = -1; break; }
            }
            send_bytes = sent_b;
        } else {
            send_bytes = -1;
        }
        pthread_mutex_unlock(&con->write_mutex);

        ASSERT(send_bytes == (int)RTCP_SR_NORB_BYTES, ({
            ERR("send (interleaved):%d:%s\n", send_bytes, strerror(errno));
            return FAILURE;}));
    } else {
        to_addr = con->addr;
        to_addr.sin_port = htons(t->client_port_rtcp);

        ASSERT((send_bytes = send(t->server_rtcp_fd,
            &(rtcp), RTCP_SR_NORB_BYTES, 0)) == (int)RTCP_SR_NORB_BYTES, ({
                    ERR("send:%d:%s\n", send_bytes, strerror(errno));
                    return FAILURE;}));
    }

    t->rtcp_packet_cnt = 0;
    t->rtcp_octet = 0;
    t->rtcp_tick = t->rtcp_tick_org;

    return SUCCESS;
}

#endif

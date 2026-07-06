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
    transport_t *t;

    ASSERT(track_id >= 0 &&
        track_id < (int)(sizeof(con->trans) / sizeof(con->trans[0])),
        return FAILURE);
    t = &con->trans[track_id];

    if (t->capture_us) {
        /* Anchor the pair to one frame: rtp_ts on the PTS media clock (the same
           clock the per-frame stamps ride) and the NTP wall-clock on that
           frame's absolute capture instant, so the receiver maps the RTP
           timeline onto absolute time with an NTP-immune delta. */
        captime_sr_ts sr = captime_sr_anchor(t->capture_pts_us, t->capture_us);
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
        ASSERT(__interleave_send(con, t->channel_rtcp, &rtcp,
            RTCP_SR_NORB_BYTES) == SUCCESS, ({
                ERR("send (interleaved):%s\n", strerror(errno));
                return FAILURE;}));
    } else {
        /* server_rtcp_fd is connect()ed to the client's RTCP port at SETUP,
           so a plain send() reaches the right peer. */
        ASSERT((send_bytes = send(t->server_rtcp_fd,
            &(rtcp), RTCP_SR_NORB_BYTES, 0)) == (int)RTCP_SR_NORB_BYTES, ({
                    ERR("send:%d:%s\n", send_bytes, strerror(errno));
                    return FAILURE;}));
    }

    /* psent/osent are cumulative totals since transmission start (RFC 3550
       6.4.1); receivers difference consecutive SRs to compute loss and rate,
       so the counters must never reset here — only the SR-interval timer does. */
    t->rtcp_tick = t->rtcp_tick_org;

    return SUCCESS;
}

#endif

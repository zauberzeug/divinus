#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

#include "rtsp_server.h"
#include "common.h"
#include "rtsp.h"
#include "list.h"
#include "hash.h"
#include "thread.h"
#include "rfc.h"
#include "rtp.h"
#include "rtcp.h"
#include "bufpool.h"
#include "mime.h"

/******************************************************************************
 *              PRIVATE DEFINITIONS
 ******************************************************************************/
//static void *rtpThrFxn(void *v);
static inline int __rtp_send(struct nal_rtp_t *rtp, struct list_head_t *trans_list);
static inline int __rtp_send_eachconnection(struct list_t *e, void *v);
static inline int __rtp_setup_transfer(struct list_t *e, void *v);
static int __rtsp_emit(void *vctx, const unsigned char *hdr, int hdr_len,
    const unsigned char *body, int body_len, int marker);
static inline int __transfer_nal_mpga(struct list_head_t *trans_list, unsigned char *ptr, size_t size);
static inline int __retrieve_sprop(rtsp_handle h, unsigned char *buf, size_t len);

struct __transfer_set_t {
    struct list_head_t list_head;
    rtsp_handle h;
    int track_id;
};

/******************************************************************************
 *              PRIVATE FUNCTIONS
 ******************************************************************************/

/* emit callback for rtp_packetize_nal: lay the FU/payload headers and body into
   one RTP packet template and fan it out to every connection. Per-connection
   sequence/timestamp/SSRC stamping (and the marker-driven AU timestamp advance)
   happen in __rtp_send_eachconnection. */
static int __rtsp_emit(void *vctx, const unsigned char *hdr, int hdr_len,
    const unsigned char *body, int body_len, int marker)
{
    struct list_head_t *trans_list = vctx;
    struct nal_rtp_t rtp;
    rtp_hdr_t *p_header = &(rtp.packet.header);
    unsigned char *payload = rtp.packet.payload;

    p_header->version = 2;
    p_header->p = 0;
    p_header->x = 0;
    p_header->cc = 0;
    p_header->pt = 96 & 0x7F;
    p_header->m = marker ? 1 : 0;

    if (hdr_len) memcpy(payload, hdr, hdr_len);
    memcpy(payload + hdr_len, body, body_len);
    rtp.rtpsize = hdr_len + body_len + sizeof(rtp_hdr_t);

    return __rtp_send(&rtp, trans_list) == SUCCESS ? 0 : -1;
}

static inline int __transfer_nal_mpga(struct list_head_t *trans_list, unsigned char *ptr, size_t size)
{
    struct nal_rtp_t rtp;

    rtp_hdr_t *p_header = &(rtp.packet.header);
    unsigned char *payload = rtp.packet.payload;

    p_header->version = 2;
    p_header->p = 0;
    p_header->x = 0;
    p_header->cc = 0;
    p_header->pt = 14;
    p_header->m = 1;

    payload[0] = payload[1] = payload[2] = payload[3] = 0;
    memcpy(payload + 4, ptr, size);
    size += 4;

    rtp.rtpsize = size + sizeof(rtp_hdr_t);

    ASSERT(__rtp_send(&rtp, trans_list) == SUCCESS, return FAILURE);

    return SUCCESS;
}

static inline int __rtp_send_eachconnection(struct list_t *e, void *v)
{
    int send_bytes;
    struct connection_item_t *con;
    struct transfer_item_t *trans;
    struct nal_rtp_t *rtp = v;
    int track_id = rtp->packet.header.pt == 96 ? 0 : 1;

    list_upcast(trans,e); 

    MUST(con = trans->con, return FAILURE);
    if (!con->trans[track_id].server_port_rtp && !con->trans[track_id].is_tcp) return SUCCESS;
    if (con->stalled || con->con_state != __CON_S_PLAYING) return SUCCESS;

    if (con->trans[track_id].au_pending) {
        unsigned int ts = (millis() * 90) & UINT32_MAX;
        if ((int)(ts - con->trans[track_id].rtp_timestamp) <= 0)
            ts = con->trans[track_id].rtp_timestamp + 1;
        con->trans[track_id].rtp_timestamp = ts;
        con->trans[track_id].au_pending = 0;
    }
    rtp->packet.header.seq = htons(con->trans[track_id].rtp_seq);
    rtp->packet.header.ts = htonl(con->trans[track_id].rtp_timestamp);
    rtp->packet.header.ssrc = htonl(con->ssrc);
    con->trans[track_id].rtp_seq += 1;
    if (rtp->packet.header.m)
        con->trans[track_id].au_pending = 1;

    if (con->trans[track_id].is_tcp) {
        unsigned char head[4];
        head[0] = '$';
        head[1] = con->trans[track_id].channel_rtp;
        head[2] = (rtp->rtpsize >> 8) & 0xFF;
        head[3] = rtp->rtpsize & 0xFF;

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
            while (sent_b < rtp->rtpsize) {
                int r = send(con->client_fd, (char*)&(rtp->packet) + sent_b, rtp->rtpsize - sent_b, 0);
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

        if (send_bytes == rtp->rtpsize) {
            con->trans[track_id].rtcp_packet_cnt += 1;
            con->trans[track_id].rtcp_octet += rtp->rtpsize;
            return SUCCESS;
        }
    } else {
        send_bytes = send(con->trans[track_id].server_rtp_fd,
            &(rtp->packet),rtp->rtpsize,0);

        if (send_bytes == rtp->rtpsize) {
            con->trans[track_id].rtcp_packet_cnt += 1;
            con->trans[track_id].rtcp_octet += rtp->rtpsize;
            return SUCCESS;
        }

        /* RTP over UDP is loss-tolerant: drop the packet rather than
           stall the encoder thread behind a full socket buffer */
        if (send_bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return SUCCESS;
    }

    /* a dead client must not abort the fan-out to the others */
    ERR("send:%d:%s\n", send_bytes, strerror(errno));
    con->stalled = 1;
    return SUCCESS;
}

static inline int __rtp_send(struct nal_rtp_t *rtp, struct list_head_t *trans_list)
{
    return list_map_inline(trans_list, (__rtp_send_eachconnection), rtp);
}


static inline int __rtp_setup_transfer(struct list_t *e, void *v)
{
    struct connection_item_t *con;
    struct __transfer_set_t *trans_set = v;
    struct transfer_item_t *trans;
    int ret = FAILURE;

    list_upcast(con,e);

    MUST(bufpool_attach(con->pool, con) == SUCCESS,
        return FAILURE);

    if (con->con_state == __CON_S_PLAYING && !con->stalled) {

        ASSERT(bufpool_get_free(trans_set->h->transfer_pool, &trans) == SUCCESS, ({
            ERR("transfer object resource starvation detected. possibly connection limits are wrongfully setup\n");
            goto error;}));

        MUST(bufpool_attach(con->pool, con) == SUCCESS,
            return FAILURE);

        trans->con = con;

        MUST(list_push(&trans_set->list_head, &trans->list_entry) == SUCCESS,
            goto error);
    }

    ret = SUCCESS;

error:
    ASSERT(bufpool_detach(con->pool, con) == SUCCESS, ret = FAILURE);

    return ret;
}

static inline int __retrieve_sprop(rtsp_handle h, unsigned char *buf, size_t len)
{
    unsigned char *nalptr;
    size_t single_len;
    mime_encoded_handle base64 = NULL;
    mime_encoded_handle base16 = NULL;

    /* check VPS is set */
    if (h->isH265 && !(h->sprop_vps_b64)) {
        nalptr = buf;
        single_len = 0;
        while (__split_nal(buf, &nalptr, &single_len, len) == SUCCESS) {
            if ((nalptr[0] >> 1 & 0x3F) == H265_NAL_TYPE_VPS) {
                ASSERT(single_len >= 4, return FAILURE);
                ASSERT(base64 = mime_base64_create((char *)&(nalptr[0]), single_len), return FAILURE);

                DASSERT(base64->base == 64, return FAILURE);

                /* optimistic lock */
                rtsp_lock(h);
                if (h->sprop_vps_b64) {
                    DBG("vps is set by another thread?\n");
                    mime_encoded_delete(base64);
                } else {
                    h->sprop_vps_b64 = base64;
                }
                rtsp_unlock(h);
            }
        }
        rtsp_lock(h);
        rtsp_unlock(h);
        base64 = NULL;
    }

    /* check SPS is set */
    if (!(h->sprop_sps_b64)) {
        nalptr = buf;
        single_len = 0;

        while (__split_nal(buf, &nalptr, &single_len, len) == SUCCESS) {
            if ((!(h->isH265) && (nalptr[0] & 0x1F) == H264_NAL_TYPE_SPS) ||
                (h->isH265 && (nalptr[0] >> 1 & 0x3F) == H265_NAL_TYPE_SPS)) {
                ASSERT(single_len >= 4, return FAILURE);
                ASSERT(base64 = mime_base64_create((char *)&(nalptr[0]), single_len), return FAILURE);
                DASSERT(base64->base == 64, return FAILURE);

                /* profile-level-id (base16 of SPS bytes) is an H.264/RFC 6184
                   SDP field only; H.265 (RFC 7798) has no such parameter. */
                if (!h->isH265) {
                    ASSERT(base16 = mime_base16_create((char *)&(nalptr[1]), 3), return FAILURE);
                    DASSERT(base16->base == 16, return FAILURE);
                }

                /* optimistic lock */
                rtsp_lock(h);
                if (h->sprop_sps_b64) {
                    DBG("sps is set by another thread?\n");
                    mime_encoded_delete(base64);
                } else {
                    h->sprop_sps_b64 = base64;
                }

                if (base16) {
                    if (h->sprop_sps_b16) {
                        DBG("sps is set by another thread?\n");
                        mime_encoded_delete(base16);
                    } else {
                        h->sprop_sps_b16 = base16;
                    }
                }
                rtsp_unlock(h);
            }
        }

        base64 = NULL;
        base16 = NULL;
    }

    /* check PPS is set */
    if (!(h->sprop_pps_b64)) {
        nalptr = buf;
        single_len = 0;
        while (__split_nal(buf, &nalptr, &single_len, len) == SUCCESS) {
            if ((!(h->isH265) && (nalptr[0] & 0x1F) == H264_NAL_TYPE_PPS) ||
                (h->isH265 && (nalptr[0] >> 1 & 0x3F) == H265_NAL_TYPE_PPS)) {
                ASSERT(single_len >= 4, return FAILURE);
                ASSERT(base64 = mime_base64_create((char *)&(nalptr[0]), single_len), return FAILURE);

                DASSERT(base64->base == 64, return FAILURE);

                /* optimistic lock */
                rtsp_lock(h);
                if (h->sprop_pps_b64) {
                    DBG("pps is set by another thread?\n");
                    mime_encoded_delete(base64);
                } else {
                    h->sprop_pps_b64 = base64;
                }
                rtsp_unlock(h);
            }
        }
        rtsp_lock(h);
        rtsp_unlock(h);
        base64 = NULL;
    }

    return SUCCESS;
}

static inline int __rtcp_poll(struct list_t *e, void *v)
{
    struct connection_item_t *con;
    struct transfer_item_t *trans;
    int *track_id = v;

    list_upcast(trans, e);
    MUST(con = trans->con, return FAILURE);

    if (con->stalled || con->con_state != __CON_S_PLAYING) return SUCCESS;
    if (!con->trans[*track_id].server_port_rtp && !con->trans[*track_id].is_tcp) return SUCCESS;

    if ((con->trans[*track_id].rtcp_tick)-- == 0) {
        if (__rtcp_send_sr(con, *track_id) != SUCCESS) {
            /* A TCP-interleaved SR failure means the data socket is dead, so
               kick; a UDP SR failure only means the client has no RTCP
               listener, so degrade RTCP for that session as before, not kick. */
            if (con->trans[*track_id].is_tcp) con->stalled = 1;
            return SUCCESS;
        }

        /* postcondition check */
        DASSERT(con->trans[*track_id].rtcp_tick == 
            con->trans[*track_id].rtcp_tick_org, return FAILURE);
        DASSERT(con->trans[*track_id].rtcp_packet_cnt == 0, return FAILURE);
        DASSERT(con->trans[*track_id].rtcp_octet == 0, return FAILURE);
    }

    return SUCCESS;
}
/******************************************************************************
 *              PUBLIC FUNCTIONS
 ******************************************************************************/
void rtp_disable_audio(rtsp_handle h)
{
    h->audioPt = 255;
}

int rtp_send_h26x(rtsp_handle h, hal_vidstream *stream, char isH265)
{
    int ret = FAILURE;
    int track_id = 0;
    struct __transfer_set_t trans = {};

    /* checkout RTP packet */
    DASSERT(h, return FAILURE);

    if (gbl_get_quit(h->pool->sharedp->gbl)) {
#ifdef DEBUG_RTSP
        ERR("server threads have gone already. call rtsp_finish()\n");
#endif
        return FAILURE;
    }

    h->isH265 = isH265;

    for (int i = 0; i < stream->count; i++) {
        ASSERT(__retrieve_sprop(h, stream->pack[i].data + stream->pack[i].offset, 
            stream->pack[i].length - stream->pack[i].offset) == SUCCESS, goto error);
    }

    trans.h = h;
    trans.track_id = track_id;

    /* setup transmission object */
    rtsp_lock(h);
    ASSERT(list_map_inline(&h->con_list, (__rtp_setup_transfer), &trans) == SUCCESS, ({rtsp_unlock(h); goto error;}));
    rtsp_unlock(h);
    
    if (trans.list_head.list) {
        for (int i = 0; i < stream->count; i++) {
            unsigned char *buf = stream->pack[i].data + stream->pack[i].offset;
            size_t len = stream->pack[i].length - stream->pack[i].offset;
            int last_pack = (i == stream->count - 1);

            /* RFC 6184/7798 payloads must not carry Annex-B start codes: split
               multi-NAL packs and packetize each NAL on its own. The marker
               rides only the last NAL of the access unit's last pack. */
            if (len >= 4 && !buf[0] && !buf[1] && !buf[2] && buf[3] == 1) {
                unsigned char *it = buf, *cur;
                size_t it_len = 0, cur_len;
                int have = __split_nal(buf, &it, &it_len, len) == SUCCESS;
                while (have) {
                    cur = it;
                    cur_len = it_len;
                    int more = __split_nal(buf, &it, &it_len, len) == SUCCESS;
                    ASSERT(rtp_packetize_nal(cur, (int)cur_len, h->isH265,
                        __RTP_MAXPAYLOADSIZE, last_pack && !more,
                        __rtsp_emit, &(trans.list_head)) == 0, goto error);
                    have = more;
                }
            } else {
                ASSERT(rtp_packetize_nal(buf, (int)len, h->isH265,
                    __RTP_MAXPAYLOADSIZE, last_pack,
                    __rtsp_emit, &(trans.list_head)) == 0, goto error);
            }
        }
        ASSERT(list_map_inline(&(trans.list_head), (__rtcp_poll), &track_id) == SUCCESS, goto error);
    }

    ret = SUCCESS;

error:
    list_destroy(&(trans.list_head));

    return ret;
}

int rtp_send_mp3(rtsp_handle h, unsigned char *buf, size_t len)
{
    int ret = FAILURE;
    int track_id = 1;
    struct __transfer_set_t trans = {};

    /* checkout RTP packet */
    DASSERT(h, return FAILURE);

    if (gbl_get_quit(h->pool->sharedp->gbl)) {
#ifdef DEBUG_RTSP
        ERR("server threads have gone already. call rtsp_finish()\n");
#endif
        return FAILURE;
    }

    h->audioPt = 14;

    trans.h = h;
    trans.track_id = track_id;

    /* setup transmission object */
    rtsp_lock(h);
    ASSERT(list_map_inline(&h->con_list, (__rtp_setup_transfer), &trans) == SUCCESS, ({rtsp_unlock(h); goto error;}));
    rtsp_unlock(h);
    
    if (trans.list_head.list) {
        ASSERT(__transfer_nal_mpga(&(trans.list_head), buf, len) == SUCCESS, goto error);
        ASSERT(list_map_inline(&(trans.list_head), (__rtcp_poll), &track_id) == SUCCESS, goto error);
    } 

    ret = SUCCESS;

error:
    list_destroy(&(trans.list_head));

    return ret;
}

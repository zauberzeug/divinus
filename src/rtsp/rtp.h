#ifndef _RTSP_RTP_H
#define _RTSP_RTP_H

#if defined (__cplusplus)
extern "C" {
#endif

#include "../hal/tools.h"
#include "../hal/captime.h"
#include "../fmt/rtppkt.h"

/******************************************************************************
 *              DEFINITIONS
 ******************************************************************************/
#define __RTP_MAXPAYLOADSIZE 1460

/******************************************************************************
 *              DATA STRUCTURES
 ******************************************************************************/
/*
 * RTP data header
 */
typedef struct {
#ifdef __RTSP_BIG_ENDIAN
    unsigned int version:2;   /* protocol version */
    unsigned int p:1;         /* padding flag */
    unsigned int x:1;         /* header extension flag */
    unsigned int cc:4;        /* CSRC count */
    unsigned int m:1;         /* marker bit */
    unsigned int pt:7;        /* payload type */
#else
    unsigned int cc:4;        /* CSRC count */
    unsigned int x:1;         /* header extension flag */
    unsigned int p:1;         /* padding flag */
    unsigned int version:2;   /* protocol version */
    unsigned int pt:7;        /* payload type */
    unsigned int m:1;         /* marker bit */
#endif
    unsigned int seq:16;      /* sequence number */
    unsigned int ts;          /* timestamp */
    unsigned int ssrc;        /* synchronization source */
    //unsigned int csrc[1];     /* optional CSRC list */
} rtp_hdr_t;

struct nal_rtp_t {
    struct {
        rtp_hdr_t header;
        /* room for a full-size payload plus the optional abs-capture-time RTP
           header extension prepended on the first packet of an access unit */
        unsigned char payload[__RTP_MAXPAYLOADSIZE + CAPTIME_ABS_CAPTURE_EXT_BYTES];
    } packet;
    int    rtpsize;
    struct list_t list_entry;
};

/******************************************************************************
 *              DECLARATIONS
 ******************************************************************************/
static inline int __split_nal(unsigned char *buf, unsigned char **nalptr, size_t *p_len, size_t max_len);

/******************************************************************************
 *              INLINE FUNCTIONS
 ******************************************************************************/
static inline int __split_nal(unsigned char *buf, unsigned char **nalptr, size_t *p_len, size_t max_len)
{
    /* SUCCESS (0) / FAILURE (-1) match nal_next's 0 / -1 contract. */
    return nal_next(buf, nalptr, p_len, max_len);
}

#if defined (__cplusplus)
}
#endif

#endif
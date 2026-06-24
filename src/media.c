#include "media.h"

#include "gain.h"
#include "stream_cfg.h"
#include "hal/captime.h"
#include "hal/sensor_mode.h"

char audioOn = 0, udpOn = 0;
pthread_mutex_t aencMtx, chnMtx, mp4Mtx;
pthread_t aencPid = 0, audPid = 0, ispPid = 0, vidPid = 0;

struct BitBuf mp3Buf;
shine_config_t mp3Cnf;
shine_t mp3Enc;
unsigned int pcmPos, pcmSamp;
short pcmSrc[SHINE_MAX_SAMPLES];

void *aenc_thread(void) {
    const uint32_t mp3FrmSize =
        (app_config.audio_srate >= 32000 ? 144 : 72) *
        (app_config.audio_bitrate * 1000) /
        app_config.audio_srate;

    while (keepRunning && audioOn) {
        pthread_mutex_lock(&aencMtx);
        if (mp3Buf.offset < mp3FrmSize) {
            pthread_mutex_unlock(&aencMtx);
            usleep(10000);
            continue;
        }

        send_mp3_to_client(mp3Buf.buf, mp3FrmSize);

        pthread_mutex_lock(&mp4Mtx);
        mp4_ingest_audio(mp3Buf.buf, mp3FrmSize);
        pthread_mutex_unlock(&mp4Mtx);

        if (app_config.rtsp_enable && rtspHandle)
            rtp_send_mp3(rtspHandle, mp3Buf.buf, mp3FrmSize);

        rtmp_ingest_audio(mp3Buf.buf, mp3FrmSize);

        mp3Buf.offset -= mp3FrmSize;
        if (mp3Buf.offset > 0)
            memmove(mp3Buf.buf, mp3Buf.buf + mp3FrmSize, mp3Buf.offset);
        pthread_mutex_unlock(&aencMtx);
    }
    HAL_INFO("media", "Shutting down audio encoding thread...\n");
}

int save_audio_stream(hal_audframe *frame) {
    int ret = EXIT_SUCCESS;

#ifdef DEBUG_AUDIO
    printf("[audio] data:%p - %02x %02x %02x %02x %02x %02x %02x %02x\n",
        frame->data, frame->data[0][0], frame->data[0][1], frame->data[0][2], frame->data[0][3],
        frame->data[0][4], frame->data[0][5], frame->data[0][6], frame->data[0][7]);
    printf("        len:%d\n", frame->length[0]);
    printf("        seq:%d\n", frame->seq);
    printf("        ts:%d\n", frame->timestamp);
#endif

    send_pcm_to_client(frame);

    unsigned int pcmLen = frame->length[0] / 2;
    short *pcmPack = (short*)frame->data[0];
    short *srcPtr = pcmPack + pcmLen;

    while (pcmPos + pcmLen >= pcmSamp) {
        int copyLen = pcmSamp - pcmPos;
        memcpy(pcmSrc + pcmPos, srcPtr - pcmLen, copyLen * 2);
        unsigned char *mp3Ptr = shine_encode_buffer_interleaved(mp3Enc, pcmSrc, &ret);
        pthread_mutex_lock(&aencMtx);
        put(&mp3Buf, mp3Ptr, ret);
        pthread_mutex_unlock(&aencMtx);
        pcmLen -= copyLen;
        pcmPos = 0;
    }

    if (pcmLen > 0)
        memcpy(pcmSrc + pcmPos, srcPtr - pcmLen, pcmLen * 2);
    pcmPos += pcmLen;

    return ret;
}

int save_video_stream(char index, hal_vidstream *stream) {
    int ret;

    /* One capture-time rebase per frame, shared by every stream below: the
       vendor PTS -> absolute epoch-µs instant. 0 if unavailable (each sender
       then keeps its send-time fallback rather than emit a wrong stamp). */
    unsigned long long capture_us = 0;
    unsigned long long pts_us = stream->count > 0 ? stream->pack[0].timestamp : 0;
    if (pts_us)
        captime_now(pts_us, &capture_us);

    switch (chnState[index].payload) {
        case HAL_VIDCODEC_H264:
        case HAL_VIDCODEC_H265:
        {
            char isH265 = chnState[index].payload == HAL_VIDCODEC_H265 ? 1 : 0;

            if (app_config.rtsp_enable && rtspHandle)
                rtp_send_h26x(rtspHandle, stream, isH265, pts_us, capture_us);

            if (app_config.stream_enable) {
                for (int i = 0; i < stream->count; i++) {
                    udp_stream_send_nal(stream->pack[i].data + stream->pack[i].offset,
                        stream->pack[i].length - stream->pack[i].offset,
                        i == stream->count - 1, isH265, pts_us);

                    rtmp_ingest_video(&stream->pack[i], isH265);
                }
            }

            if (app_config.mp4_enable) {
                send_h26x_to_client(index, stream);

                pthread_mutex_lock(&mp4Mtx);
                send_mp4_to_client(index, stream, isH265);
                if (recordOn) send_mp4_to_record(stream, isH265);
                pthread_mutex_unlock(&mp4Mtx);
            }
            break;
        }
        case HAL_VIDCODEC_MJPG:
            if (app_config.mjpeg_enable) {
                static char *mjpeg_buf;
                static ssize_t mjpeg_buf_size = 0;
                ssize_t buf_size = 0;
                for (unsigned int i = 0; i < stream->count; i++) {
                    hal_vidpack *data = &stream->pack[i];
                    ssize_t need_size = buf_size + data->length - data->offset + 2;
                    if (need_size > mjpeg_buf_size)
                        mjpeg_buf = realloc(mjpeg_buf, mjpeg_buf_size = need_size);
                    memcpy(mjpeg_buf + buf_size, data->data + data->offset,
                        data->length - data->offset);
                    buf_size += data->length - data->offset;
                }
                send_mjpeg_to_client(index, mjpeg_buf, buf_size, capture_us);
            }
            break;
        case HAL_VIDCODEC_JPG:
        {
            static char *jpeg_buf;
            static ssize_t jpeg_buf_size = 0;
            ssize_t buf_size = 0;
            for (unsigned int i = 0; i < stream->count; i++) {
                hal_vidpack *data = &stream->pack[i];
                ssize_t need_size = buf_size + data->length - data->offset + 2;
                if (need_size > jpeg_buf_size)
                    jpeg_buf = realloc(jpeg_buf, jpeg_buf_size = need_size);
                memcpy(jpeg_buf + buf_size, data->data + data->offset,
                    data->length - data->offset);
                buf_size += data->length - data->offset;
            }
            if (app_config.jpeg_enable)
                send_jpeg_to_client(index, jpeg_buf, buf_size);
            break;
        }
        default:
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int media_start(void) {
    int ret = EXIT_SUCCESS;

    for (int i = 0; app_config.stream_dests[i] && *app_config.stream_dests[i]; i++) {
        if (STARTS_WITH(app_config.stream_dests[i], "rtmp://")) {
            flv_set_config(app_config.mp4_width, app_config.mp4_height, app_config.mp4_fps,
                app_config.audio_enable ? HAL_AUDCODEC_MP3 : HAL_AUDCODEC_UNSPEC,
                app_config.audio_bitrate, 1, app_config.audio_srate);
            rtmp_init(app_config.stream_dests[i]);
            continue;
        }

        if (STARTS_WITH(app_config.stream_dests[i], "udp://")) {
            char host[INET_ADDRSTRLEN];
            unsigned short port;
            int mcast;
            if (stream_parse_dest(app_config.stream_dests[i], host, sizeof(host),
                    &port, &mcast)) {
                HAL_DANGER("media", "Invalid UDP destination: %s, skipping!\n",
                    app_config.stream_dests[i]);
                continue;
            }

            if (!udpOn) {
                if (udp_stream_init(app_config.stream_udp_srcport, mcast ? host : NULL))
                    return EXIT_FAILURE;
                udpOn = 1;
            }

            if (udp_stream_add_client(host, port) != -1)
                HAL_INFO("media", "Starting streaming to %s...\n", app_config.stream_dests[i]);
        }
    }

    return ret;
}

void media_stop(void) {
    if (udpOn) {
        udp_stream_close();
        udpOn = 0;
    }
    rtmp_close();
}

void media_stream_sync(void) {
    /* Runtime enable/disable of the UDP push via /api/stream, no process restart.
       Disabling is handled entirely by the save_video_stream() fan-out, which
       only feeds the sender while app_config.stream_enable is set — so we must
       NOT tear the context down here: udp_stream_send_nal() runs on the encoder
       thread and would use-after-free a context freed from the HTTP thread. The
       context is brought up once and lives until media_stop() at shutdown (after
       sdk_stop() has quiesced the encoder). Enabling just (re)points it at the
       configured destination; the source port takes effect on the next start. */
    if (!app_config.stream_enable || !*app_config.stream_dests[0]) return;

    char host[INET_ADDRSTRLEN];
    unsigned short port;
    int mcast;
    if (stream_parse_dest(app_config.stream_dests[0], host, sizeof(host),
            &port, &mcast)) {
        HAL_DANGER("media", "Invalid UDP destination: %s\n",
            app_config.stream_dests[0]);
        return;
    }
    if (!udpOn) {
        if (udp_stream_init(app_config.stream_udp_srcport, mcast ? host : NULL))
            return;
        udpOn = 1;
    }
    udp_stream_set_client(host, port);
}

void request_idr(void) {
    signed char index = -1;
    pthread_mutex_lock(&chnMtx);
    for (int i = 0; i < chnCount; i++) {
        if (!chnState[i].enable) continue;
        if (chnState[i].payload != HAL_VIDCODEC_H264 &&
            chnState[i].payload != HAL_VIDCODEC_H265) continue;
        index = i;
        break;
    }
    if (index != -1) switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  i6_video_request_idr(index); break;
        case HAL_PLATFORM_I6C: i6c_video_request_idr(index); break;
        case HAL_PLATFORM_M6:  m6_video_request_idr(index); break;
        case HAL_PLATFORM_RK:  rk_video_request_idr(index); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_video_request_idr(index); break;
        case HAL_PLATFORM_GM:  gm_video_request_idr(index); break;
        case HAL_PLATFORM_V1:  v1_video_request_idr(index); break;
        case HAL_PLATFORM_V2:  v2_video_request_idr(index); break;
        case HAL_PLATFORM_V3:  v3_video_request_idr(index); break;
        case HAL_PLATFORM_V4:  v4_video_request_idr(index); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_video_request_idr(index); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_video_request_idr(index); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);
}

void set_grayscale(bool active) {
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  i6_channel_grayscale(active); break;
        case HAL_PLATFORM_I6C: i6c_channel_grayscale(active); break;
        case HAL_PLATFORM_M6:  m6_channel_grayscale(active); break;
        case HAL_PLATFORM_RK:  rk_channel_grayscale(active); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_channel_grayscale(active); break;
        case HAL_PLATFORM_V1:  v1_channel_grayscale(active); break;
        case HAL_PLATFORM_V2:  v2_channel_grayscale(active); break;
        case HAL_PLATFORM_V3:  v3_channel_grayscale(active); break;
        case HAL_PLATFORM_V4:  v4_channel_grayscale(active); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_channel_grayscale(active); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_channel_grayscale(active); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);
}

static unsigned int get_frame_budget_us(void) {
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  return i6_sensor_frame_budget();
#endif
    }
    return 0;
}

int set_exposure(unsigned int micros) {
    int ret = -1;
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  ret = i6_sensor_exposure(micros); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);

    /* Latch a fixed request down to the frame budget: the frame rate is the
       contract, so the stored value reflects what it actually allows and
       never silently rises if the rate is later lowered. */
    if (!ret && app_config.exposure != 0 && app_config.exposure != EXPOSURE_MAX) {
        unsigned int budget = get_frame_budget_us();
        if (budget && app_config.exposure > budget)
            app_config.exposure = budget;
    }
    return ret;
}

static int set_sensor_rate(char framerate) {
    int ret = EXIT_SUCCESS;
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  ret = i6_sensor_set_rate(framerate); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);
    return ret;
}

/* The sensor rate is the highest enabled stream rate; recompute it (and the
   exposure policy that hangs off the frame budget) when a stream's fps or
   enable state changes at runtime, so the UI's stream settings take effect
   without a restart. */
void refresh_sensor_rate(void) {
    char target = MAX(app_config.mp4_enable ? app_config.mp4_fps : 0,
                      app_config.mjpeg_enable ? app_config.mjpeg_fps : 0);
    if (target <= 0)
        return;
    /* Re-pin exposure unconditionally: on success it re-derives against the new
       budget; on a failed rate change the rate is unchanged, so it still
       restores a correct shutter (undoing the transient narrowing). */
    set_sensor_rate(target);
    set_exposure(app_config.exposure);
}

int get_gain_limits(hal_gainlimits *limits) {
    int ret = -1;
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  ret = i6_sensor_gain_limits_get(limits); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);
    return ret;
}

int set_gain_limits(const hal_gainlimits *request) {
    int ret = -1;
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6: {
            hal_gainlimits merged;
            if (ret = i6_sensor_gain_limits_get(&merged)) break;
            if (ret = gain_limits_merge(&merged, request)) break;
            ret = i6_sensor_gain_limits_set(&merged);
            break;
        }
#endif
    }
    pthread_mutex_unlock(&chnMtx);
    return ret;
}

int get_ae_state(hal_aestate *state) {
    int ret = -1;
    pthread_mutex_lock(&chnMtx);
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  ret = i6_sensor_ae_query(state); break;
#endif
    }
    pthread_mutex_unlock(&chnMtx);
    return ret;
}

int take_next_free_channel(bool mainLoop) {
    pthread_mutex_lock(&chnMtx);
    for (int i = 0; i < chnCount; i++) {
        if (chnState[i].enable) continue;
        chnState[i].enable = true;
        chnState[i].mainLoop = mainLoop;
        pthread_mutex_unlock(&chnMtx);
        return i;
    }
    pthread_mutex_unlock(&chnMtx);
    return -1;
}

int create_channel(char index, short width, short height, char framerate, char jpeg) {
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  return i6_channel_create(index, width, height, jpeg);
        case HAL_PLATFORM_I6C: return i6c_channel_create(index, width, height, jpeg);
        case HAL_PLATFORM_M6:  return m6_channel_create(index, width, height, jpeg);
        case HAL_PLATFORM_RK:  return rk_channel_create(index, width, height,
            app_config.mirror, app_config.flip);
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  return EXIT_SUCCESS;
        case HAL_PLATFORM_GM:  return EXIT_SUCCESS;
        case HAL_PLATFORM_V1:  return v1_channel_create(index, width, height,
            app_config.mirror, app_config.flip, framerate);
        case HAL_PLATFORM_V2:  return v2_channel_create(index, width, height,
            app_config.mirror, app_config.flip, framerate);
        case HAL_PLATFORM_V3:  return v3_channel_create(index, width, height,
            app_config.mirror, app_config.flip, framerate);
        case HAL_PLATFORM_V4:  return v4_channel_create(index, app_config.mirror,
            app_config.flip, framerate);
#elif defined(__mips__)
        case HAL_PLATFORM_T31: return t31_channel_create(index, width, height,
            framerate, jpeg);
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: return cvi_channel_create(index, width, height,
            app_config.mirror, app_config.flip);
#endif
    }
    return EXIT_FAILURE;
}

int bind_channel(char index, char framerate, char jpeg) {
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  return i6_channel_bind(index, framerate);
        case HAL_PLATFORM_I6C: return i6c_channel_bind(index, framerate);
        case HAL_PLATFORM_M6:  return m6_channel_bind(index, framerate);
        case HAL_PLATFORM_RK:  return rk_channel_bind(index);
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  return ak_channel_bind(index);
        case HAL_PLATFORM_GM:  return gm_channel_bind(index);
        case HAL_PLATFORM_V1:  return v1_channel_bind(index);
        case HAL_PLATFORM_V2:  return v2_channel_bind(index);
        case HAL_PLATFORM_V3:  return v3_channel_bind(index);
        case HAL_PLATFORM_V4:  return v4_channel_bind(index);
#elif defined(__mips__)
        case HAL_PLATFORM_T31: return t31_channel_bind(index);
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: return cvi_channel_bind(index);
#endif
    }
    return EXIT_FAILURE;
}

int unbind_channel(char index, char jpeg) {
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  return i6_channel_unbind(index);
        case HAL_PLATFORM_I6C: return i6c_channel_unbind(index);
        case HAL_PLATFORM_M6:  return m6_channel_unbind(index);
        case HAL_PLATFORM_RK:  return rk_channel_unbind(index);
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  return ak_channel_unbind(index);
        case HAL_PLATFORM_GM:  return gm_channel_unbind(index);
        case HAL_PLATFORM_V1:  return v1_channel_unbind(index);
        case HAL_PLATFORM_V2:  return v2_channel_unbind(index);
        case HAL_PLATFORM_V3:  return v3_channel_unbind(index);
        case HAL_PLATFORM_V4:  return v4_channel_unbind(index);
#elif defined(__mips__)
        case HAL_PLATFORM_T31: return t31_channel_unbind(index);
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: return cvi_channel_unbind(index);
#endif
    }
    // Unmatched platform: nothing was bound, so teardown is a no-op
    return 0;
}

int media_video_disable(char index, char jpeg) {
    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  return i6_video_destroy(index);
        case HAL_PLATFORM_I6C: return i6c_video_destroy(index);
        case HAL_PLATFORM_M6:  return m6_video_destroy(index);
        case HAL_PLATFORM_RK:  return rk_video_destroy(index);
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  return ak_video_destroy(index);
        case HAL_PLATFORM_GM:  return gm_video_destroy(index);
        case HAL_PLATFORM_V1:  return v1_video_destroy(index);
        case HAL_PLATFORM_V2:  return v2_video_destroy(index);
        case HAL_PLATFORM_V3:  return v3_video_destroy(index);
        case HAL_PLATFORM_V4:  return v4_video_destroy(index);
#elif defined(__mips__)
        case HAL_PLATFORM_T31: return t31_video_destroy(index);
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: return cvi_video_destroy(index);
#endif
    }
    return 0;
}

void media_audio_disable(void) {
    if (!audioOn) return;

    audioOn = 0;

    pthread_join(aencPid, NULL);
    pthread_join(audPid, NULL);
    shine_close(mp3Enc);

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  i6_audio_deinit(); break;
        case HAL_PLATFORM_I6C: i6c_audio_deinit(); break;
        case HAL_PLATFORM_M6:  m6_audio_deinit(); break;
        case HAL_PLATFORM_RK:  rk_audio_deinit(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_GM:  gm_audio_deinit(); break;
        case HAL_PLATFORM_V1:  v1_audio_deinit(); break;
        case HAL_PLATFORM_V2:  v2_audio_deinit(); break;
        case HAL_PLATFORM_V3:  v3_audio_deinit(); break;
        case HAL_PLATFORM_V4:  v4_audio_deinit(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_audio_deinit(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_audio_deinit(); break;
#endif
    }
}

int media_audio_enable(void) {
    int ret = EXIT_SUCCESS;

    if (audioOn) return ret;

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  ret = i6_audio_init(app_config.audio_srate, app_config.audio_gain); break;
        case HAL_PLATFORM_I6C: ret = i6c_audio_init(app_config.audio_srate, app_config.audio_gain); break;
        case HAL_PLATFORM_M6:  ret = m6_audio_init(app_config.audio_srate, app_config.audio_gain); break;
        case HAL_PLATFORM_RK:  ret = rk_audio_init(app_config.audio_srate); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_GM:  ret = gm_audio_init(app_config.audio_srate); break;
        case HAL_PLATFORM_V1:  ret = v1_audio_init(app_config.audio_srate); break;
        case HAL_PLATFORM_V2:  ret = v2_audio_init(app_config.audio_srate); break;
        case HAL_PLATFORM_V3:  ret = v3_audio_init(app_config.audio_srate); break;
        case HAL_PLATFORM_V4:  ret = v4_audio_init(app_config.audio_srate); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: ret = t31_audio_init(app_config.audio_srate); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: ret = cvi_audio_init(app_config.audio_srate); break;
#endif
    }
    if (ret)
        HAL_ERROR("media", "Audio initialization failed with %#x!\n%s\n",
            ret, errstr(ret));

    if (shine_check_config(app_config.audio_srate, app_config.audio_bitrate) < 0)
        HAL_ERROR("media", "MP3 samplerate/bitrate configuration is unsupported!\n");
    else {
        mp3Cnf.mpeg.mode = MONO;
        mp3Cnf.mpeg.bitr = app_config.audio_bitrate;
        mp3Cnf.mpeg.emph = NONE;
        mp3Cnf.mpeg.copyright = 0;
        mp3Cnf.mpeg.original = 1;
        mp3Cnf.wave.channels = PCM_MONO;
        mp3Cnf.wave.samplerate = app_config.audio_srate;
        if (!(mp3Enc = shine_initialise(&mp3Cnf)))
            HAL_ERROR("media", "MP3 encoder initialization failed!\n");

        pcmSamp = shine_samples_per_pass(mp3Enc);
    }

    {
        pthread_attr_t thread_attr;
        pthread_attr_init(&thread_attr);
        size_t stacksize;
        pthread_attr_getstacksize(&thread_attr, &stacksize);
        size_t new_stacksize = 16384;
        if (pthread_attr_setstacksize(&thread_attr, new_stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", new_stacksize);
        if (pthread_create(
                        &audPid, &thread_attr, (void *(*)(void *))aud_thread, NULL))
            HAL_ERROR("media", "Starting the audio capture thread failed!\n");
        if (pthread_attr_setstacksize(&thread_attr, stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", stacksize);
        pthread_attr_destroy(&thread_attr);
    }

    {
        pthread_attr_t thread_attr;
        pthread_attr_init(&thread_attr);
        size_t stacksize;
        pthread_attr_getstacksize(&thread_attr, &stacksize);
        size_t new_stacksize = 16384;
        if (pthread_attr_setstacksize(&thread_attr, new_stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", new_stacksize);
        if (pthread_create(
                        &aencPid, &thread_attr, (void *(*)(void *))aenc_thread, NULL))
            HAL_ERROR("media", "Starting the audio encoding thread failed!\n");
        if (pthread_attr_setstacksize(&thread_attr, stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", stacksize);
        pthread_attr_destroy(&thread_attr);
    }

    audioOn = 1;

    return ret;
}

int media_mjpeg_disable(void) {
    int ret;

    for (char i = 0; i < chnCount; i++) {
        if (!chnState[i].enable) continue;
        if (chnState[i].payload != HAL_VIDCODEC_MJPG) continue;

        if (ret = unbind_channel(i, 1))
            HAL_ERROR("media", "Unbinding channel %d failed with %#x!\n%s\n",
                i, ret, errstr(ret));

        if (ret = media_video_disable(i, 1))
            HAL_ERROR("media", "Disabling encoder %d failed with %#x!\n%s\n",
                i, ret, errstr(ret));
    }

    return EXIT_SUCCESS;
}

int media_mjpeg_enable(void) {
    int ret;

    int index = take_next_free_channel(true);

    if (ret = create_channel(index, app_config.mjpeg_width,
        app_config.mjpeg_height, app_config.mjpeg_fps, 1))
        HAL_ERROR("media", "Creating channel %d failed with %#x!\n%s\n",
            index, ret, errstr(ret));

    {
        hal_vidconfig config;
        config.width = app_config.mjpeg_width;
        config.height = app_config.mjpeg_height;
        config.codec = HAL_VIDCODEC_MJPG;
        config.mode = app_config.mjpeg_mode;
        config.framerate = app_config.mjpeg_fps;
        config.bitrate = app_config.mjpeg_bitrate;
        config.maxBitrate = app_config.mjpeg_bitrate * 5 / 4;
        config.minQual = config.maxQual = app_config.mjpeg_qfactor ?
            app_config.mjpeg_qfactor : app_config.jpeg_qfactor;

        switch (plat) {
#if defined(__ARM_PCS_VFP)
            case HAL_PLATFORM_I6:  ret = i6_video_create(index, &config); break;
            case HAL_PLATFORM_I6C: ret = i6c_video_create(index, &config); break;
            case HAL_PLATFORM_M6:  ret = m6_video_create(index, &config); break;
            case HAL_PLATFORM_RK:  ret = rk_video_create(index, &config); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
            case HAL_PLATFORM_AK:  ret = ak_video_create(index, &config); break;
            case HAL_PLATFORM_GM:  ret = gm_video_create(index, &config); break;
            case HAL_PLATFORM_V1:  ret = v1_video_create(index, &config); break;
            case HAL_PLATFORM_V2:  ret = v2_video_create(index, &config); break;
            case HAL_PLATFORM_V3:  ret = v3_video_create(index, &config); break;
            case HAL_PLATFORM_V4:  ret = v4_video_create(index, &config); break;
#elif defined(__mips__)
            case HAL_PLATFORM_T31: ret = t31_video_create(index, &config); break;
#elif defined(__riscv) || defined(__riscv__)
            case HAL_PLATFORM_CVI: ret = cvi_video_create(index, &config); break;
#endif
        }

        if (ret)
            HAL_ERROR("media", "Creating encoder %d failed with %#x!\n%s\n",
                index, ret, errstr(ret));
    }

    if (ret = bind_channel(index, app_config.mjpeg_fps, 1))
        HAL_ERROR("media", "Binding channel %d failed with %#x!\n%s\n",
            index, ret, errstr(ret));

    return EXIT_SUCCESS;
}

int media_mp4_disable(void) {
    int ret;

    /* The H26x encoder (and thus its SPS/PPS/VPS) is going away; drop the
       cached RTSP parameter sets up front so a reconfig never serves stale ones
       in the SDP, even if a teardown step below errors out early. */
    if (app_config.rtsp_enable && rtspHandle)
        rtsp_clear_sprops(rtspHandle);

    for (char i = 0; i < chnCount; i++) {
        if (!chnState[i].enable) continue;
        if (chnState[i].payload != HAL_VIDCODEC_H264 &&
            chnState[i].payload != HAL_VIDCODEC_H265) continue;

        if (ret = unbind_channel(i, 1))
            HAL_ERROR("media", "Unbinding channel %d failed with %#x!\n%s\n",
                i, ret, errstr(ret));

        if (ret = media_video_disable(i, 1))
            HAL_ERROR("media", "Disabling encoder %d failed with %#x!\n%s\n",
                i, ret, errstr(ret));
    }

    return EXIT_SUCCESS;
}

int media_mp4_enable(void) {
    int ret;

    int index = take_next_free_channel(true);

    if (ret = create_channel(index, app_config.mp4_width,
        app_config.mp4_height, app_config.mp4_fps, 0))
        HAL_ERROR("media", "Creating channel %d failed with %#x!\n%s\n",
            index, ret, errstr(ret));

    {
        hal_vidconfig config;
        config.width = app_config.mp4_width;
        config.height = app_config.mp4_height;
        config.codec = app_config.mp4_codecH265 ?
            HAL_VIDCODEC_H265 : HAL_VIDCODEC_H264;
        config.mode = app_config.mp4_mode;
        config.profile = app_config.mp4_profile;
        config.gop = app_config.mp4_gop;
        config.framerate = app_config.mp4_fps;
        config.bitrate = app_config.mp4_bitrate;
        config.maxBitrate = app_config.mp4_bitrate * 5 / 4;
        config.minQual = 34;
        config.maxQual = 48;

        switch (plat) {
#if defined(__ARM_PCS_VFP)
            case HAL_PLATFORM_I6:  ret = i6_video_create(index, &config); break;
            case HAL_PLATFORM_I6C: ret = i6c_video_create(index, &config); break;
            case HAL_PLATFORM_M6:  ret = m6_video_create(index, &config); break;
            case HAL_PLATFORM_RK:  ret = rk_video_create(index, &config); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
            case HAL_PLATFORM_AK:  ret = ak_video_create(index, &config); break;
            case HAL_PLATFORM_GM:  ret = gm_video_create(index, &config); break;
            case HAL_PLATFORM_V1:  ret = v1_video_create(index, &config); break;
            case HAL_PLATFORM_V2:  ret = v2_video_create(index, &config); break;
            case HAL_PLATFORM_V3:  ret = v3_video_create(index, &config); break;
            case HAL_PLATFORM_V4:  ret = v4_video_create(index, &config); break;
#elif defined(__mips__)
            case HAL_PLATFORM_T31: ret = t31_video_create(index, &config); break;
#elif defined(__riscv) || defined(__riscv__)
            case HAL_PLATFORM_CVI: ret = cvi_video_create(index, &config); break;
#endif
        }

        if (ret)
            HAL_ERROR("media", "Creating encoder %d failed with %#x!\n%s\n",
                index, ret, errstr(ret));

        mp4_set_config(app_config.mp4_width, app_config.mp4_height, app_config.mp4_fps,
            app_config.audio_enable ? HAL_AUDCODEC_MP3 : HAL_AUDCODEC_UNSPEC,
            app_config.audio_bitrate, 1, app_config.audio_srate);
    }

    if (ret = bind_channel(index, app_config.mp4_fps, 0))
        HAL_ERROR("media", "Binding channel %d failed with %#x!\n%s\n",
            index, ret, errstr(ret));

    return EXIT_SUCCESS;
}

int sdk_start(void) {
    int ret = EXIT_SUCCESS;

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I3:  ret = i3_hal_init(); break;
        case HAL_PLATFORM_I6:  ret = i6_hal_init(); break;
        case HAL_PLATFORM_I6C: ret = i6c_hal_init(); break;
        case HAL_PLATFORM_M6:  ret = m6_hal_init(); break;
        case HAL_PLATFORM_RK:  ret = rk_hal_init(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ret = ak_hal_init(); break;
        case HAL_PLATFORM_GM:  ret = gm_hal_init(); break;
        case HAL_PLATFORM_V1:  ret = v1_hal_init(); break;
        case HAL_PLATFORM_V2:  ret = v2_hal_init(); break;
        case HAL_PLATFORM_V3:  ret = v3_hal_init(); break;
        case HAL_PLATFORM_V4:  ret = v4_hal_init(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: ret = t31_hal_init(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: ret = cvi_hal_init(); break;
#endif
        default: HAL_ERROR("media", "Unsupported platform!\n");
    }
    if (ret)
        HAL_ERROR("media", "HAL initialization failed with %#x!\n%s\n",
            ret, errstr(ret));

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:
            i6_aud_cb = save_audio_stream;
            i6_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_I6C:
            i6c_aud_cb = save_audio_stream;
            i6c_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_M6:
            m6_aud_cb = save_audio_stream;
            m6_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_RK:
            rk_aud_cb = save_audio_stream;
            rk_vid_cb = save_video_stream;
            break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_GM:
            gm_aud_cb = save_audio_stream;
            gm_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_V1:
            v1_aud_cb = save_audio_stream;
            v1_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_V2:
            v2_aud_cb = save_audio_stream;
            v2_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_V3:
            v3_aud_cb = save_audio_stream;
            v3_vid_cb = save_video_stream;
            break;
        case HAL_PLATFORM_V4:
            v4_aud_cb = save_audio_stream;
            v4_vid_cb = save_video_stream;
            break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31:
            t31_aud_cb = save_audio_stream;
            t31_vid_cb = save_video_stream;
            break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI:
            cvi_aud_cb = save_audio_stream;
            cvi_vid_cb = save_video_stream;
            break;
#endif
    }

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I3:  ret = i3_system_init(); break;
        case HAL_PLATFORM_I6:  ret = i6_system_init(); break;
        case HAL_PLATFORM_I6C: ret = i6c_system_init(); break;
        case HAL_PLATFORM_M6:  ret = m6_system_init(); break;
        case HAL_PLATFORM_RK:  ret = rk_system_init(0); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ret = ak_system_init(app_config.sensor_config); break;
        case HAL_PLATFORM_GM:  ret = gm_system_init(); break;
        case HAL_PLATFORM_V1:  ret = v1_system_init(app_config.sensor_config); break;
        case HAL_PLATFORM_V2:  ret = v2_system_init(app_config.sensor_config); break;
        case HAL_PLATFORM_V3:  ret = v3_system_init(app_config.sensor_config); break;
        case HAL_PLATFORM_V4:  ret = v4_system_init(app_config.sensor_config); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: ret = t31_system_init(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: ret = cvi_system_init(app_config.sensor_config); break;
#endif
    }
    if (ret)
        HAL_ERROR("media", "System initialization failed with %#x!\n%s\n",
            ret, errstr(ret));

    if (app_config.audio_enable) {
        ret = media_audio_enable();
        if (ret)
            HAL_ERROR("media", "Audio initialization failed with %#x!\n%s\n",
                ret, errstr(ret));
    }

    short width = MAX(app_config.mp4_enable ? app_config.mp4_width : 0,
        app_config.mjpeg_enable ? app_config.mjpeg_width : 0);
    short height = MAX(app_config.mp4_enable ? app_config.mp4_height : 0,
        app_config.mjpeg_enable ? app_config.mjpeg_height : 0);
    short framerate = MAX(app_config.mp4_enable ? app_config.mp4_fps : 0,
        app_config.mjpeg_enable ? app_config.mjpeg_fps : 0);
    if (!app_config.mp4_enable && !app_config.mjpeg_enable) {
        width = 640;
        height = 480;
        framerate = 15;
    }

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:
            _i6_level3dnr = app_config.level3dnr;
            ret = i6_pipeline_create(0, width,
            height, app_config.mirror, app_config.flip, framerate,
            app_config.sensor_profile); break;
        case HAL_PLATFORM_I6C:
            _i6_level3dnr = app_config.level3dnr;
            ret = i6c_pipeline_create(0, width,
            height, app_config.mirror, app_config.flip, framerate,
            app_config.sensor_profile); break;
        case HAL_PLATFORM_M6:
            _i6_level3dnr = app_config.level3dnr;
            ret = m6_pipeline_create(0, width,
            height, app_config.mirror, app_config.flip, framerate,
            app_config.sensor_profile); break;
        case HAL_PLATFORM_RK:  ret = rk_pipeline_create(width, height); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ret = ak_pipeline_create(app_config.mirror,
            app_config.flip); break;
        case HAL_PLATFORM_GM:  ret = gm_pipeline_create(app_config.mirror,
            app_config.flip); break;
        case HAL_PLATFORM_V1:  ret = v1_pipeline_create(); break;
        case HAL_PLATFORM_V2:  ret = v2_pipeline_create(); break;
        case HAL_PLATFORM_V3:  ret = v3_pipeline_create(); break;
        case HAL_PLATFORM_V4:  ret = v4_pipeline_create(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: ret = t31_pipeline_create(app_config.mirror,
            app_config.flip, app_config.antiflicker, framerate); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: ret = cvi_pipeline_create(); break;
#endif
    }
    if (ret)
        HAL_ERROR("media", "Pipeline creation failed with %#x!\n%s\n",
            ret, errstr(ret));

    if (isp_thread) {
        pthread_attr_t thread_attr;
        pthread_attr_init(&thread_attr);
        size_t stacksize;
        pthread_attr_getstacksize(&thread_attr, &stacksize);
        size_t new_stacksize = app_config.isp_thread_stack_size;
        if (pthread_attr_setstacksize(&thread_attr, new_stacksize))
            HAL_DANGER("media", "Can't set stack size %zu!\n", new_stacksize);
        if (pthread_create(
                     &ispPid, &thread_attr, (void *(*)(void *))isp_thread, NULL))
            HAL_ERROR("media", "Starting the imaging thread failed!\n");
        if (pthread_attr_setstacksize(&thread_attr, stacksize))
            HAL_DANGER("media", "Can't set stack size %zu!\n", stacksize);
        pthread_attr_destroy(&thread_attr);
    }

    if (app_config.mp4_enable && (ret = media_mp4_enable()))
        HAL_ERROR("media", "MP4 initialization failed with %#x!\n", ret);

    if (app_config.mjpeg_enable && (ret = media_mjpeg_enable()))
        HAL_ERROR("media", "MJPEG initialization failed with %#x!\n", ret);

    if (app_config.jpeg_enable && (ret = jpeg_init()))
        HAL_ERROR("media", "JPEG initialization failed with %#x!\n", ret);

    {
        pthread_attr_t thread_attr;
        pthread_attr_init(&thread_attr);
        size_t stacksize;
        pthread_attr_getstacksize(&thread_attr, &stacksize);
        size_t new_stacksize = app_config.venc_stream_thread_stack_size;
        if (pthread_attr_setstacksize(&thread_attr, new_stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", new_stacksize);
        if (pthread_create(
                     &vidPid, &thread_attr, (void *(*)(void *))vid_thread, NULL))
            HAL_ERROR("media", "Starting the video encoding thread failed!\n");
        if (pthread_attr_setstacksize(&thread_attr, stacksize))
            HAL_DANGER("media", "Can't set stack size %zu\n", stacksize);
        pthread_attr_destroy(&thread_attr);
    }

    if (!access(app_config.sensor_config, F_OK) && !sleep(1))
        switch (plat) {
#if defined(__ARM_PCS_VFP)
            case HAL_PLATFORM_I3:  i3_config_load(app_config.sensor_config); break;
            case HAL_PLATFORM_I6:  i6_config_load(app_config.sensor_config); break;
            case HAL_PLATFORM_I6C: i6c_config_load(app_config.sensor_config); break;
            case HAL_PLATFORM_M6:  m6_config_load(app_config.sensor_config); break;
#elif defined(__mips__)
            case HAL_PLATFORM_T31: t31_config_load(app_config.sensor_config); break;
#endif
        }

#if defined(__ARM_PCS_VFP)
    // Once the SigmaStar pipeline is streaming, log the sensor's advertised
    // resolution modes from /proc. (Reading the node mid-bring-up disturbs the
    // sensor, and the vendor fnGetResolution call can't walk the table; see
    // hal/sensor_mode.c.)
    if (plat == HAL_PLATFORM_I6 || plat == HAL_PLATFORM_I6C ||
        plat == HAL_PLATFORM_M6) {
        sensor_mode modes[SENSOR_MODE_MAX];
        int listed = sensor_mode_read(0, modes, SENSOR_MODE_MAX);
        if (listed > 0) {
            sensor_mode_log("sensor", modes, listed);
            sensor_mode_cache(modes, listed);
        }
    }
#endif

    /* Apply the exposure policy for the configured frame rate, including the
       auto default so its shutter ceiling is tied to the frame budget. */
    ret = set_exposure(app_config.exposure);
    if (ret) {
        if (app_config.exposure)
            HAL_DANGER("media", "Failed to set exposure: %#x\n", ret);
    } else if (app_config.exposure == 0)
        HAL_INFO("media", "Exposure: auto (shutter capped to frame budget)\n");
    else if (app_config.exposure == EXPOSURE_MAX)
        HAL_INFO("media", "Exposure: max (pinned to full frame time)\n");
    else
        HAL_INFO("media", "Exposure: fixed %uus (clamped to frame budget)\n",
            app_config.exposure);

    {
        hal_gainlimits request = {
            .minSensorGain = app_config.min_gain,
            .maxSensorGain = app_config.max_gain,
            .minIspGain = app_config.min_isp_gain,
            .maxIspGain = app_config.max_isp_gain };
        if (request.minSensorGain || request.maxSensorGain ||
            request.minIspGain || request.maxIspGain) {
            ret = set_gain_limits(&request);
            if (ret)
                HAL_DANGER("media", "Failed to set gain limits: %#x\n", ret);
            else
                HAL_INFO("media", "Gain limits set to sensor %u..%u, "
                    "isp %u..%u (1024 = 1x, 0 = unchanged)\n",
                    request.minSensorGain, request.maxSensorGain,
                    request.minIspGain, request.maxIspGain);
        }
    }

    HAL_INFO("media", "SDK has started successfully!\n");

    return EXIT_SUCCESS;
}

int sdk_stop(void) {
    pthread_join(vidPid, NULL);

    if (app_config.jpeg_enable)
        jpeg_deinit();

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  i6_video_destroy_all(); break;
        case HAL_PLATFORM_I6C: i6c_video_destroy_all(); break;
        case HAL_PLATFORM_M6:  m6_video_destroy_all(); break;
        case HAL_PLATFORM_RK:  rk_video_destroy_all(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_video_destroy_all(); break;
        case HAL_PLATFORM_GM:  gm_video_destroy_all(); break;
        case HAL_PLATFORM_V1:  v1_video_destroy_all(); break;
        case HAL_PLATFORM_V2:  v2_video_destroy_all(); break;
        case HAL_PLATFORM_V3:  v3_video_destroy_all(); break;
        case HAL_PLATFORM_V4:  v4_video_destroy_all(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_video_destroy_all(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_video_destroy_all(); break;
#endif
    }

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I6:  i6_pipeline_destroy(); break;
        case HAL_PLATFORM_I6C: i6c_pipeline_destroy(); break;
        case HAL_PLATFORM_M6:  m6_pipeline_destroy(); break;
        case HAL_PLATFORM_RK:  rk_pipeline_destroy(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_pipeline_destroy(); break;
        case HAL_PLATFORM_GM:  gm_pipeline_destroy(); break;
        case HAL_PLATFORM_V1:  v1_pipeline_destroy(); break;
        case HAL_PLATFORM_V2:  v2_pipeline_destroy(); break;
        case HAL_PLATFORM_V3:  v3_pipeline_destroy(); break;
        case HAL_PLATFORM_V4:  v4_pipeline_destroy(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_pipeline_destroy(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_pipeline_destroy(); break;
#endif
    }

    if (app_config.audio_enable)
        media_audio_disable();

    if (isp_thread)
        pthread_join(ispPid, NULL);

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I3:  i3_system_deinit(); break;
        case HAL_PLATFORM_I6:  i6_system_deinit(); break;
        case HAL_PLATFORM_I6C: i6c_system_deinit(); break;
        case HAL_PLATFORM_M6:  m6_system_deinit(); break;
        case HAL_PLATFORM_RK:  rk_system_deinit(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_system_deinit(); break;
        case HAL_PLATFORM_GM:  gm_system_deinit(); break;
        case HAL_PLATFORM_V1:  v1_system_deinit(); break;
        case HAL_PLATFORM_V2:  v2_system_deinit(); break;
        case HAL_PLATFORM_V3:  v3_system_deinit(); break;
        case HAL_PLATFORM_V4:  v4_system_deinit(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_system_deinit(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_system_deinit(); break;
#endif
    }

    switch (plat) {
#if defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_V1: v1_sensor_deinit(); break;
        case HAL_PLATFORM_V2: v2_sensor_deinit(); break;
        case HAL_PLATFORM_V3: v3_sensor_deinit(); break;
        case HAL_PLATFORM_V4: v4_sensor_deinit(); break;
#endif
    }

    switch (plat) {
#if defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_I3:  i3_hal_deinit(); break;
        case HAL_PLATFORM_I6:  i6_hal_deinit(); break;
        case HAL_PLATFORM_I6C: i6c_hal_deinit(); break;
        case HAL_PLATFORM_M6:  m6_hal_deinit(); break;
        case HAL_PLATFORM_RK:  rk_hal_deinit(); break;
#elif defined(__arm__) && !defined(__ARM_PCS_VFP)
        case HAL_PLATFORM_AK:  ak_hal_deinit(); break;
        case HAL_PLATFORM_GM:  gm_hal_deinit(); break;
        case HAL_PLATFORM_V1:  v1_hal_deinit(); break;
        case HAL_PLATFORM_V2:  v2_hal_deinit(); break;
        case HAL_PLATFORM_V3:  v3_hal_deinit(); break;
        case HAL_PLATFORM_V4:  v4_hal_deinit(); break;
#elif defined(__mips__)
        case HAL_PLATFORM_T31: t31_hal_deinit(); break;
#elif defined(__riscv) || defined(__riscv__)
        case HAL_PLATFORM_CVI: cvi_hal_deinit(); break;
#endif
    }

    HAL_INFO("media", "SDK had stopped successfully!\n");
    return EXIT_SUCCESS;
}

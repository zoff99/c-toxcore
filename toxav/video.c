/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "video.h"

#include "msi.h"
#include "ring_buffer.h"
#include "rtp.h"

#include "../toxcore/logger.h"
#include "../toxcore/network.h"

/*
 * Zoff: disable logging in ToxAV for now
 */

#include <stdio.h>

#undef LOGGER_DEBUG
#define LOGGER_DEBUG(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_ERROR
#define LOGGER_ERROR(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_WARNING
#define LOGGER_WARNING(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_INFO
#define LOGGER_INFO(log, ...) printf(__VA_ARGS__);printf("\n")

/**
 * Soft deadline the decoder should attempt to meet, in "us" (microseconds).
 * Set to zero for unlimited.
 *
 * By convention, the value 1 is used to mean "return as fast as possible."
 */
// TODO(zoff99): don't hardcode this, let the application choose it
#define WANTED_MAX_DECODER_FPS 40

/**
 * VPX_DL_REALTIME       (1)
 * deadline parameter analogous to VPx REALTIME mode.
 *
 * VPX_DL_GOOD_QUALITY   (1000000)
 * deadline parameter analogous to VPx GOOD QUALITY mode.
 *
 * VPX_DL_BEST_QUALITY   (0)
 * deadline parameter analogous to VPx BEST QUALITY mode.
 */
#define MAX_DECODE_TIME_US (1000000 / WANTED_MAX_DECODER_FPS) // to allow x fps

/**
 * Codec control function to set encoder internal speed settings. Changes in
 * this value influences, among others, the encoder's selection of motion
 * estimation methods. Values greater than 0 will increase encoder speed at the
 * expense of quality.
 *
 * Note Valid range for VP8: -16..16
 */
#define VP8E_SET_CPUUSED_VALUE 16

/**
 * Initialize encoder with this value. Target bandwidth to use for this stream, in kilobits per second.
 */
#define VIDEO_BITRATE_INITIAL_VALUE 5000
#define VIDEO_DECODE_BUFFER_SIZE 5 // this buffer has normally max. 1 entry

static vpx_codec_iface_t *video_codec_decoder_interface(void)
{
    return vpx_codec_vp8_dx();
}
static vpx_codec_iface_t *video_codec_encoder_interface(void)
{
    return vpx_codec_vp8_cx();
}

#define VIDEO_CODEC_DECODER_MAX_WIDTH  800 // its a dummy value, because the struct needs a value there
#define VIDEO_CODEC_DECODER_MAX_HEIGHT 600 // its a dummy value, because the struct needs a value there

#define VPX_MAX_DIST_START 40

#define VPX_MAX_ENCODER_THREADS 4
#define VPX_MAX_DECODER_THREADS 4
#define VIDEO_VP8_DECODER_POST_PROCESSING_ENABLED 0

static void vc_init_encoder_cfg(const Logger *log, vpx_codec_enc_cfg_t *cfg, int16_t kf_max_dist)
{
    vpx_codec_err_t rc = vpx_codec_enc_config_default(video_codec_encoder_interface(), cfg, 0);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "vc_init_encoder_cfg:Failed to get config: %s", vpx_codec_err_to_string(rc));
    }

    /* Target bandwidth to use for this stream, in kilobits per second */
    cfg->rc_target_bitrate = VIDEO_BITRATE_INITIAL_VALUE;
    cfg->g_w = VIDEO_CODEC_DECODER_MAX_WIDTH;
    cfg->g_h = VIDEO_CODEC_DECODER_MAX_HEIGHT;
    cfg->g_pass = VPX_RC_ONE_PASS;
    cfg->g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT | VPX_ERROR_RESILIENT_PARTITIONS;
    cfg->g_lag_in_frames = 0;

    /* Allow lagged encoding
     *
     * If set, this value allows the encoder to consume a number of input
     * frames before producing output frames. This allows the encoder to
     * base decisions for the current frame on future frames. This does
     * increase the latency of the encoding pipeline, so it is not appropriate
     * in all situations (ex: realtime encoding).
     *
     * Note that this is a maximum value -- the encoder may produce frames
     * sooner than the given limit. Set this value to 0 to disable this
     * feature.
     */
    cfg->kf_min_dist = 0;
    cfg->kf_mode = VPX_KF_AUTO; // Encoder determines optimal placement automatically
    cfg->rc_end_usage = VPX_VBR; // what quality mode?

    /*
     * VPX_VBR    Variable Bit Rate (VBR) mode
     * VPX_CBR    Constant Bit Rate (CBR) mode
     * VPX_CQ     Constrained Quality (CQ) mode -> give codec a hint that we may be on low bandwidth connection
     * VPX_Q    Constant Quality (Q) mode
     */
    if (kf_max_dist > 1) {
        cfg->kf_max_dist = kf_max_dist; // a full frame every x frames minimum (can be more often, codec decides automatically)
        LOGGER_DEBUG(log, "kf_max_dist=%d (1)", cfg->kf_max_dist);
    } else {
        cfg->kf_max_dist = VPX_MAX_DIST_START;
        LOGGER_DEBUG(log, "kf_max_dist=%d (2)", cfg->kf_max_dist);
    }

    cfg->g_threads = VPX_MAX_ENCODER_THREADS; // Maximum number of threads to use
    /* TODO: set these to something reasonable */
    // cfg->g_timebase.num = 1;
    // cfg->g_timebase.den = 60; // 60 fps
    cfg->rc_resize_allowed = 1; // allow encoder to resize to smaller resolution
    cfg->rc_resize_up_thresh = 40;
    cfg->rc_resize_down_thresh = 5;

#define MAX_DECODE_TIME_US 0 /* Good quality encode. */
#define VIDEO_DECODE_BUFFER_SIZE 20

VCSession *vc_new(Logger *log, ToxAV *av, uint32_t friend_number, toxav_video_receive_frame_cb *cb, void *cb_data)
{
    VCSession *vc = (VCSession *)calloc(sizeof(VCSession), 1);
    vpx_codec_err_t rc;

    if (!vc) {
        LOGGER_WARNING(log, "Allocation failed! Application might misbehave!");
        return NULL;
    }

    if (create_recursive_mutex(vc->queue_mutex) != 0) {
        LOGGER_WARNING(log, "Failed to create recursive mutex!");
        free(vc);
        return NULL;
    }

    if (!(vc->vbuf_raw = rb_new(VIDEO_DECODE_BUFFER_SIZE))) {
        goto BASE_CLEANUP;
    }

    rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE, NULL, 0);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Init video_decoder failed: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP;
    }

    /* Set encoder to some initial values
     */
    vpx_codec_enc_cfg_t  cfg;
    rc = vpx_codec_enc_config_default(VIDEO_CODEC_ENCODER_INTERFACE, &cfg, 0);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to get config: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP_1;
    }

    cfg.rc_target_bitrate = 500000;
    cfg.g_w = 800;
    cfg.g_h = 600;
    cfg.g_pass = VPX_RC_ONE_PASS;
    /* TODO(mannol): If we set error resilience the app will crash due to bug in vp8.
       Perhaps vp9 has solved it?*/
#if 0
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT | VPX_ERROR_RESILIENT_PARTITIONS;
#endif
    cfg.g_lag_in_frames = 0;
    cfg.kf_min_dist = 0;
    cfg.kf_max_dist = 48;
    cfg.kf_mode = VPX_KF_AUTO;

    rc = vpx_codec_enc_init(vc->encoder, VIDEO_CODEC_ENCODER_INTERFACE, &cfg, 0);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to initialize encoder: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP_1;
    }

    rc = vpx_codec_control(vc->encoder, VP8E_SET_CPUUSED, 8);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    }

    /*
     * VPX_CTRL_USE_TYPE(VP8E_SET_NOISE_SENSITIVITY,  unsigned int)
     * control function to set noise sensitivity
     *   0: off, 1: OnYOnly, 2: OnYUV, 3: OnYUVAggressive, 4: Adaptive
     */
#if 0
    rc = vpx_codec_control(vc->encoder, VP8E_SET_NOISE_SENSITIVITY, 2);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    }

#endif
    vc->linfts = current_time_monotonic(mono_time);
    vc->lcfd = 60;
    vc->vcb = cb;
    vc->vcb_user_data = cb_data;
    vc->friend_number = friend_number;
    vc->av = av;
    vc->log = log;

    return vc;

BASE_CLEANUP_1:
    vpx_codec_destroy(vc->decoder);
BASE_CLEANUP:
    pthread_mutex_destroy(vc->queue_mutex);
    rb_kill(vc->vbuf_raw);
    free(vc);
    return NULL;
}

void vc_kill(VCSession *vc)
{
    if (!vc) {
        return;
    }

    vpx_codec_destroy(vc->encoder);
    vpx_codec_destroy(vc->decoder);

    void *p;
    uint8_t dummy;

    while (rb_read((RingBuffer *)vc->vbuf_raw, &p, &dummy)) {
        free(p);
    }

    rb_kill((RingBuffer *)vc->vbuf_raw);

    pthread_mutex_destroy(vc->queue_mutex);

    LOGGER_DEBUG(vc->log, "Terminated video handler: %p", vc);
    free(vc);
}


void vc_iterate(VCSession *vc)
{
    if (!vc)
    {
        return;
    }

    pthread_mutex_lock(vc->queue_mutex);

    struct RTPMessage *p;

    if (!rb_read(vc->vbuf_raw, (void **)&p)) {
        LOGGER_TRACE(vc->log, "no Video frame data available");
        pthread_mutex_unlock(vc->queue_mutex);
        return;
    }

    uint16_t log_rb_size = rb_size(vc->vbuf_raw);
    pthread_mutex_unlock(vc->queue_mutex);
    const struct RTPHeader *const header = &p->header;

    uint32_t full_data_len;

    if (rb_read((RingBuffer *)vc->vbuf_raw, (void **)&p, &data_type))
    {
        pthread_mutex_unlock(vc->queue_mutex);

        const struct RTPHeaderV3 *header_v3 = (void *)&(p->header);
        if ( ((uint8_t)header_v3->protocol_version) == 3)
        {
            full_data_len = header_v3->data_length_full;
        }
        else
        {
            full_data_len = p->len;
        }

        LOGGER_DEBUG(vc->log, "vc_iterate: rb_read p->len=%d data_type=%d", (int)full_data_len, (int)data_type);
        LOGGER_DEBUG(vc->log, "vc_iterate: rb_read rb size=%d", (int)rb_size((RingBuffer *)vc->vbuf_raw));

        rc = vpx_codec_decode(vc->decoder, p->data, full_data_len, NULL, MAX_DECODE_TIME_US);
        if (rc != VPX_CODEC_OK)
        {
            if (rc == 5) // Bitstream not supported by this decoder
            {
                LOGGER_WARNING(vc->log, "Switching VPX Decoder");
                // video_switch_decoder(vc);
            }
            else if (rc == 7)
            {
                LOGGER_WARNING(vc->log, "Corrupt frame detected: data size=%d start byte=%d end byte=%d",
                    (int)full_data_len, (int)p->data[0], (int)p->data[full_data_len - 1]);
            }
            else
            {
                LOGGER_ERROR(vc->log, "Error decoding video: %d %s", (int)rc, vpx_codec_err_to_string(rc));
            }

            rc = vpx_codec_decode(vc->decoder, p->data, full_data_len, NULL, MAX_DECODE_TIME_US);
			if (rc != 5)
			{
				LOGGER_ERROR(vc->log, "There is still an error decoding video: %d %s", (int)rc, vpx_codec_err_to_string(rc));
			}
        }

    LOGGER_DEBUG(vc->log, "vc_iterate: rb_read p->len=%d p->header.xe=%d", (int)full_data_len, p->header.xe);
    LOGGER_DEBUG(vc->log, "vc_iterate: rb_read rb size=%d", (int)log_rb_size);
    const vpx_codec_err_t rc = vpx_codec_decode(vc->decoder, p->data, full_data_len, nullptr, MAX_DECODE_TIME_US);

    free(p);
        return;
    }
    else
    {
        LOGGER_DEBUG(vc->log, "Error decoding video: rb_read");
    }
}


int vc_queue_message(void *vcp, struct RTPMessage *msg)
{
    /* This function is called with complete messages
     * they have already been assembled.
     * this function gets called from handle_rtp_packet() and handle_rtp_packet_v3() 
     */
    if (!vcp || !msg) {
        if (msg) {
            free(msg);
        }

        return -1;
    }

    VCSession *vc = (VCSession *)vcp;

    // const struct RTPHeader *header = (void *)&(msg->header);
    const struct RTPHeaderV3 *header_v3 = (void *)&(msg->header);

    if (msg->header.pt == (RTP_TYPE_VIDEO + 2) % 128) {
        LOGGER_WARNING(vc->log, "Got dummy!");
        free(msg);
        return 0;
    }

    if (msg->header.pt != rtp_TypeVideo % 128) {
        LOGGER_WARNING(vc->log, "Invalid payload type!");
        free(msg);
        return -1;
    }

    pthread_mutex_lock(vc->queue_mutex);

    if (( ((uint8_t)header_v3->protocol_version) == 3) &&
        ( ((uint8_t)header_v3->pt) == rtp_TypeVideo)
        )
    {
        free(rb_write((RingBuffer *)vc->vbuf_raw, msg, (uint8_t)header_v3->is_keyframe));
    }
    else
    {
        free(rb_write((RingBuffer *)vc->vbuf_raw, msg, 0));
    }


    /* Calculate time it took for peer to send us this frame */
    uint32_t t_lcfd = current_time_monotonic(mono_time) - vc->linfts;
    vc->lcfd = t_lcfd > 100 ? vc->lcfd : t_lcfd;
    vc->linfts = current_time_monotonic();

    pthread_mutex_unlock(vc->queue_mutex);

    return 0;
}

int vc_reconfigure_encoder(VCSession *vc, uint32_t bit_rate, uint16_t width, uint16_t height)
{
    if (!vc) {
        return -1;
    }

    vpx_codec_enc_cfg_t cfg = *vc->encoder->config.enc;
    vpx_codec_err_t rc;

    if (cfg.rc_target_bitrate == bit_rate && cfg.g_w == width && cfg.g_h == height) {
        return 0; /* Nothing changed */
    }

    if (cfg.g_w == width && cfg.g_h == height) {
        /* Only bit rate changed */
        cfg.rc_target_bitrate = bit_rate;

        rc = vpx_codec_enc_config_set(vc->encoder, &cfg);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
            return -1;
        }
    } else {
        /* Resolution is changed, must reinitialize encoder since libvpx v1.4 doesn't support
         * reconfiguring encoder to use resolutions greater than initially set.
         */

        LOGGER_DEBUG(vc->log, "Have to reinitialize vpx encoder on session %p", vc);

        cfg.rc_target_bitrate = bit_rate;
        cfg.g_w = width;
        cfg.g_h = height;

        vpx_codec_ctx_t new_c;

        rc = vpx_codec_enc_init(&new_c, VIDEO_CODEC_ENCODER_INTERFACE, &cfg, 0);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Failed to initialize encoder: %s", vpx_codec_err_to_string(rc));
            return -1;
        }

        rc = vpx_codec_control(&new_c, VP8E_SET_CPUUSED, 8);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
            vpx_codec_destroy(&new_c);
            return -1;
        }

        vpx_codec_destroy(vc->encoder);
        memcpy(vc->encoder, &new_c, sizeof(new_c));
    }

    return 0;
}

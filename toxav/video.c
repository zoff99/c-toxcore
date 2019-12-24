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
#include "ts_buffer.h"
#include "rtp.h"

#include "../toxcore/logger.h"
#include "../toxcore/network.h"
#include "../toxcore/Messenger.h"
#include "../toxcore/mono_time.h"


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

/*
  Soft deadline the decoder should attempt to meet, in "us" (microseconds). Set to zero for unlimited.
  By convention, the value 1 is used to mean "return as fast as possible."
*/
// TODO: don't hardcode this, let the application choose it
#define WANTED_MAX_DECODER_FPS (20)
#define MAX_DECODE_TIME_US (1000000 / WANTED_MAX_DECODER_FPS) // to allow x fps
/*
VPX_DL_REALTIME       (1)
deadline parameter analogous to VPx REALTIME mode.

VPX_DL_GOOD_QUALITY   (1000000)
deadline parameter analogous to VPx GOOD QUALITY mode.

VPX_DL_BEST_QUALITY   (0)
deadline parameter analogous to VPx BEST QUALITY mode.
*/


// initialize encoder with this value. Target bandwidth to use for this stream, in kilobits per second.
#define VIDEO_BITRATE_INITIAL_VALUE 400
#define VIDEO_BITRATE_INITIAL_VALUE_VP9 400

struct vpx_frame_user_data {
    uint64_t record_timestamp;
};


uint32_t MaxIntraTarget(uint32_t optimalBuffersize)
{
    // Set max to the optimal buffer level (normalized by target BR),
    // and scaled by a scalePar.
    // Max target size = scalePar * optimalBufferSize * targetBR[Kbps].
    // This values is presented in percentage of perFrameBw:
    // perFrameBw = targetBR[Kbps] * 1000 / frameRate.
    // The target in % is as follows:
    float scalePar = 0.5;
    float codec_maxFramerate = 30;
    uint32_t targetPct = optimalBuffersize * scalePar * codec_maxFramerate / 10;

    // Don't go below 3 times the per frame bandwidth.
    const uint32_t minIntraTh = 300;
    return (targetPct < minIntraTh) ? minIntraTh : targetPct;
}


void vc__init_encoder_cfg(Logger *log, vpx_codec_enc_cfg_t *cfg, int16_t kf_max_dist, int32_t quality,
                          int32_t rc_max_quantizer, int32_t rc_min_quantizer, int32_t encoder_codec,
                          int32_t video_keyframe_method)
{

    vpx_codec_err_t rc;

    if (encoder_codec != TOXAV_ENCODER_CODEC_USED_VP9) {
        LOGGER_WARNING(log, "Using VP8 codec for encoder (1)");
        rc = vpx_codec_enc_config_default(VIDEO_CODEC_ENCODER_INTERFACE_VP8, cfg, 0);
    } else {
        LOGGER_WARNING(log, "Using VP9 codec for encoder (1)");
        rc = vpx_codec_enc_config_default(VIDEO_CODEC_ENCODER_INTERFACE_VP9, cfg, 0);
    }

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "vc__init_encoder_cfg:Failed to get config: %s", vpx_codec_err_to_string(rc));
    }

    if (encoder_codec == TOXAV_ENCODER_CODEC_USED_VP9) {
        cfg->rc_target_bitrate = VIDEO_BITRATE_INITIAL_VALUE_VP9;
    } else {
        cfg->rc_target_bitrate =
            VIDEO_BITRATE_INITIAL_VALUE; /* Target bandwidth to use for this stream, in kilobits per second */
    }

    cfg->g_w = VIDEO_CODEC_DECODER_MAX_WIDTH;
    cfg->g_h = VIDEO_CODEC_DECODER_MAX_HEIGHT;
    cfg->g_pass = VPX_RC_ONE_PASS;



    /* zoff (in 2017) */
    cfg->g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT; // | VPX_ERROR_RESILIENT_PARTITIONS;
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

    if (video_keyframe_method == TOXAV_ENCODER_KF_METHOD_PATTERN) {
        cfg->kf_min_dist = 0;
        cfg->kf_mode = VPX_KF_DISABLED;
    } else {
        cfg->kf_min_dist = 0;
        cfg->kf_mode = VPX_KF_AUTO; // Encoder determines optimal placement automatically
    }

    cfg->rc_end_usage = VPX_VBR; // VPX_VBR; // what quality mode?
    /*
     VPX_VBR    Variable Bit Rate (VBR) mode
     VPX_CBR    Constant Bit Rate (CBR) mode
     VPX_CQ     Constrained Quality (CQ) mode -> give codec a hint that we may be on low bandwidth connection
     VPX_Q      Constant Quality (Q) mode
     */

    if (kf_max_dist > 1) {
        cfg->kf_max_dist = kf_max_dist; // a full frame every x frames minimum (can be more often, codec decides automatically)
        LOGGER_WARNING(log, "kf_max_dist=%d (1)", cfg->kf_max_dist);
    } else {
        cfg->kf_max_dist = VPX_MAX_DIST_START;
        LOGGER_WARNING(log, "kf_max_dist=%d (2)", cfg->kf_max_dist);
    }

    if (encoder_codec == TOXAV_ENCODER_CODEC_USED_VP9) {
        cfg->kf_max_dist = VIDEO__VP9_KF_MAX_DIST;
        LOGGER_WARNING(log, "kf_max_dist=%d (3)", cfg->kf_max_dist);
    }

    cfg->g_threads = VPX_MAX_ENCODER_THREADS; // Maximum number of threads to use

    cfg->g_timebase.num = 1; // 0.1 ms = timebase units = (1/10000)s
    cfg->g_timebase.den = 10000; // 0.1 ms = timebase units = (1/10000)s

    if (encoder_codec == TOXAV_ENCODER_CODEC_USED_VP9) {
        cfg->rc_dropframe_thresh = 0;
        cfg->rc_resize_allowed = 0;
    } else {
        if (quality == TOXAV_ENCODER_VP8_QUALITY_HIGH) {
            /* Highest-resolution encoder settings */
            cfg->rc_dropframe_thresh = 0; // 0
            cfg->rc_resize_allowed = 0; // 0
            cfg->rc_min_quantizer = rc_min_quantizer; // 0
            cfg->rc_max_quantizer = rc_max_quantizer; // 40
            cfg->rc_resize_up_thresh = 29;
            cfg->rc_resize_down_thresh = 5;
            cfg->rc_undershoot_pct = 100; // 100
            cfg->rc_overshoot_pct = 15; // 15
            cfg->rc_buf_initial_sz = 500; // 500 in ms
            cfg->rc_buf_optimal_sz = 600; // 600 in ms
            cfg->rc_buf_sz = 1000; // 1000 in ms
        } else { // TOXAV_ENCODER_VP8_QUALITY_NORMAL
            cfg->rc_dropframe_thresh = 0;
            cfg->rc_resize_allowed = 0; // allow encoder to resize to smaller resolution
            cfg->rc_min_quantizer = rc_min_quantizer; // 2
            cfg->rc_max_quantizer = rc_max_quantizer; // 63
            cfg->rc_resize_up_thresh = TOXAV_ENCODER_VP_RC_RESIZE_UP_THRESH;
            cfg->rc_resize_down_thresh = TOXAV_ENCODER_VP_RC_RESIZE_DOWN_THRESH;
            cfg->rc_undershoot_pct = 100; // 100
            cfg->rc_overshoot_pct = 15; // 15
            cfg->rc_buf_initial_sz = 500; // 500 in ms
            cfg->rc_buf_optimal_sz = 600; // 600 in ms
            cfg->rc_buf_sz = 1000; // 1000 in ms
        }

    }
}

VCSession *vc_new_h264(Logger *log, ToxAV *av, uint32_t friend_number, toxav_video_receive_frame_cb *cb, void *cb_data,
                       VCSession *vc)
{

    // ENCODER -------
    x264_param_t param;

    if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) {
        // goto fail;
    }

    /* Configure non-default params */
    // param.i_bitdepth = 8;
    param.i_csp = X264_CSP_I420;
    param.i_width  = 1920;
    param.i_height = 1080;
    vc->h264_enc_width = param.i_width;
    vc->h264_enc_height = param.i_height;
    param.i_threads = 3;
    param.b_deterministic = 0;
    param.b_intra_refresh = 1;
    // param.b_open_gop = 20;
    param.i_keyint_max = VIDEO_MAX_KF_H264;
    // param.rc.i_rc_method = X264_RC_CRF; // X264_RC_ABR;
    // param.i_nal_hrd = X264_NAL_HRD_CBR;

    param.b_vfr_input = 1; /* VFR input.  If 1, use timebase and timestamps for ratecontrol purposes.
                            * If 0, use fps only. */
    param.i_timebase_num = 1;       // 1 ms = timebase units = (1/1000)s
    param.i_timebase_den = 1000;   // 1 ms = timebase units = (1/1000)s
    param.b_repeat_headers = 1;
    param.b_annexb = 1;

    param.rc.f_rate_tolerance = VIDEO_F_RATE_TOLERANCE_H264;
    param.rc.i_vbv_buffer_size = VIDEO_BITRATE_INITIAL_VALUE_H264 * VIDEO_BUF_FACTOR_H264;
    param.rc.i_vbv_max_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * 1;
    // param.rc.i_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * VIDEO_BITRATE_FACTOR_H264;

    vc->h264_enc_bitrate = VIDEO_BITRATE_INITIAL_VALUE_H264 * 1000;

    param.rc.b_stat_read = 0;
    param.rc.b_stat_write = 1;
    x264_param_apply_fastfirstpass(&param);

    /* Apply profile restrictions. */
    if (x264_param_apply_profile(&param,
                                 x264_param_profile_str) < 0) { // "baseline", "main", "high", "high10", "high422", "high444"
        // goto fail;
    }

    if (x264_picture_alloc(&(vc->h264_in_pic), param.i_csp, param.i_width, param.i_height) < 0) {
        // goto fail;
    }

    // vc->h264_in_pic.img.plane[0] --> Y
    // vc->h264_in_pic.img.plane[1] --> U
    // vc->h264_in_pic.img.plane[2] --> V

    vc->h264_encoder = x264_encoder_open(&param);

//    goto good;

//fail:
//    vc->h264_encoder = NULL;

//good:

    // ENCODER -------


    // DECODER -------
    AVCodec *codec;
    vc->h264_decoder = NULL;

    avcodec_register_all();

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);


    if (!codec) {
        LOGGER_WARNING(log, "codec not found H264 on decoder");
    }

    vc->h264_decoder = avcodec_alloc_context3(codec);

    if (codec->capabilities & CODEC_CAP_TRUNCATED) {
        vc->h264_decoder->flags |= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
    }

    vc->h264_decoder->refcounted_frames = 0;
    /*   When AVCodecContext.refcounted_frames is set to 0, the returned
    *             reference belongs to the decoder and is valid only until the
    *             next call to this function or until closing or flushing the
    *             decoder. The caller may not write to it.
    */

    if (avcodec_open2(vc->h264_decoder, codec, NULL) < 0) {
        LOGGER_WARNING(log, "could not open codec H264 on decoder");
    }

    vc->h264_decoder->refcounted_frames = 0;
    /*   When AVCodecContext.refcounted_frames is set to 0, the returned
    *             reference belongs to the decoder and is valid only until the
    *             next call to this function or until closing or flushing the
    *             decoder. The caller may not write to it.
    */

    // DECODER -------

    return vc;
}

VCSession *vc_new_vpx(Logger *log, ToxAV *av, uint32_t friend_number, toxav_video_receive_frame_cb *cb, void *cb_data,
                      VCSession *vc)
{

    vpx_codec_err_t rc;
    vc->send_keyframe_request_received = 0;
    vc->h264_video_capabilities_received = 0;

    /*
    VPX_CODEC_USE_FRAME_THREADING
       Enable frame-based multi-threading

    VPX_CODEC_USE_ERROR_CONCEALMENT
       Conceal errors in decoded frames
    */
    vpx_codec_dec_cfg_t  dec_cfg;
    dec_cfg.threads = VPX_MAX_DECODER_THREADS; // Maximum number of threads to use
    dec_cfg.w = VIDEO_CODEC_DECODER_MAX_WIDTH;
    dec_cfg.h = VIDEO_CODEC_DECODER_MAX_HEIGHT;

    if (VPX_DECODER_USED != TOXAV_ENCODER_CODEC_USED_VP9) {
        LOGGER_WARNING(log, "Using VP8 codec for decoder (0)");

        vpx_codec_flags_t dec_flags_ = 0;

        if (vc->video_decoder_error_concealment == 1) {
            vpx_codec_caps_t decoder_caps = vpx_codec_get_caps(VIDEO_CODEC_DECODER_INTERFACE_VP8);

            if (decoder_caps & VPX_CODEC_CAP_ERROR_CONCEALMENT) {
                dec_flags_ = VPX_CODEC_USE_ERROR_CONCEALMENT;
                LOGGER_WARNING(log, "Using VP8 VPX_CODEC_USE_ERROR_CONCEALMENT (0)");
            }
        }

#ifdef VIDEO_CODEC_ENCODER_USE_FRAGMENTS
        rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE_VP8, &dec_cfg,
                                dec_flags_ | VPX_CODEC_USE_FRAME_THREADING
                                | VPX_CODEC_USE_POSTPROC | VPX_CODEC_USE_INPUT_FRAGMENTS);
        LOGGER_WARNING(log, "Using VP8 using input fragments (0) rc=%d", (int)rc);
#else
        rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE_VP8, &dec_cfg,
                                dec_flags_ | VPX_CODEC_USE_FRAME_THREADING
                                | VPX_CODEC_USE_POSTPROC);
#endif

        if (rc == VPX_CODEC_INCAPABLE) {
            LOGGER_WARNING(log, "Postproc not supported by this decoder (0)");
            rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE_VP8, &dec_cfg,
                                    dec_flags_ | VPX_CODEC_USE_FRAME_THREADING);
        }

    } else {
        LOGGER_WARNING(log, "Using VP9 codec for decoder (0)");
        rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE_VP9, &dec_cfg,
                                VPX_CODEC_USE_FRAME_THREADING);
    }

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Init video_decoder failed: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP;
    }


    if (VPX_DECODER_USED != TOXAV_ENCODER_CODEC_USED_VP9) {
        if (VIDEO__VP8_DECODER_POST_PROCESSING_ENABLED == 1) {
            LOGGER_WARNING(log, "turn on postproc: OK");
        } else if (VIDEO__VP8_DECODER_POST_PROCESSING_ENABLED == 2) {
            vp8_postproc_cfg_t pp = {VP8_MFQE, 1, 0};
            vpx_codec_err_t cc_res = vpx_codec_control(vc->decoder, VP8_SET_POSTPROC, &pp);

            if (cc_res != VPX_CODEC_OK) {
                LOGGER_WARNING(log, "Failed to turn on postproc");
            } else {
                LOGGER_WARNING(log, "turn on postproc: OK");
            }
        } else if (VIDEO__VP8_DECODER_POST_PROCESSING_ENABLED == 3) {
            vp8_postproc_cfg_t pp = {VP8_DEBLOCK | VP8_DEMACROBLOCK | VP8_MFQE, 1, 0};
            vpx_codec_err_t cc_res = vpx_codec_control(vc->decoder, VP8_SET_POSTPROC, &pp);

            if (cc_res != VPX_CODEC_OK) {
                LOGGER_WARNING(log, "Failed to turn on postproc");
            } else {
                LOGGER_WARNING(log, "turn on postproc: OK");
            }
        } else {
            vp8_postproc_cfg_t pp = {0, 0, 0};
            vpx_codec_err_t cc_res = vpx_codec_control(vc->decoder, VP8_SET_POSTPROC, &pp);

            if (cc_res != VPX_CODEC_OK) {
                LOGGER_WARNING(log, "Failed to turn OFF postproc");
            } else {
                LOGGER_WARNING(log, "Disable postproc: OK");
            }
        }
    }










    /* Set encoder to some initial values
     */
    vpx_codec_enc_cfg_t  cfg;
    vc__init_encoder_cfg(log, &cfg, 1,
                         vc->video_encoder_vp8_quality,
                         vc->video_rc_max_quantizer,
                         vc->video_rc_min_quantizer,
                         vc->video_encoder_coded_used,
                         vc->video_keyframe_method);

    if (vc->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_VP9) {
        LOGGER_WARNING(log, "Using VP8 codec for encoder (0.1)");

#ifdef VIDEO_CODEC_ENCODER_USE_FRAGMENTS
        rc = vpx_codec_enc_init(vc->encoder, VIDEO_CODEC_ENCODER_INTERFACE_VP8, &cfg,
                                VPX_CODEC_USE_FRAME_THREADING | VPX_CODEC_USE_OUTPUT_PARTITION);
#else
        rc = vpx_codec_enc_init(vc->encoder, VIDEO_CODEC_ENCODER_INTERFACE_VP8, &cfg,
                                VPX_CODEC_USE_FRAME_THREADING);
#endif

    } else {
        LOGGER_WARNING(log, "Using VP9 codec for encoder (0.1)");
        rc = vpx_codec_enc_init(vc->encoder, VIDEO_CODEC_ENCODER_INTERFACE_VP9, &cfg, VPX_CODEC_USE_FRAME_THREADING);
    }

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to initialize encoder: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP_1;
    }


#if 1
    rc = vpx_codec_control(vc->encoder, VP8E_SET_ENABLEAUTOALTREF, 0);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder VP8E_SET_ENABLEAUTOALTREF setting: %s value=%d", vpx_codec_err_to_string(rc),
                     (int)0);
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    } else {
        LOGGER_WARNING(log, "set encoder VP8E_SET_ENABLEAUTOALTREF setting: %s value=%d", vpx_codec_err_to_string(rc),
                       (int)0);
    }

#endif


#if 1
    uint32_t rc_max_intra_target = MaxIntraTarget(600);
    rc_max_intra_target = 102;
    rc = vpx_codec_control(vc->encoder, VP8E_SET_MAX_INTRA_BITRATE_PCT, rc_max_intra_target);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder VP8E_SET_MAX_INTRA_BITRATE_PCT setting: %s value=%d",
                     vpx_codec_err_to_string(rc),
                     (int)rc_max_intra_target);
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    } else {
        LOGGER_WARNING(log, "set encoder VP8E_SET_MAX_INTRA_BITRATE_PCT setting: %s value=%d", vpx_codec_err_to_string(rc),
                       (int)rc_max_intra_target);
    }

#endif



    /*
    Codec control function to set encoder internal speed settings.
    Changes in this value influences, among others, the encoder's selection of motion estimation methods.
    Values greater than 0 will increase encoder speed at the expense of quality.

    Note:
      Valid range for VP8: -16..16
      Valid range for VP9: -8..8
    */

    int cpu_used_value = vc->video_encoder_cpu_used;

    if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9) {
        if (cpu_used_value < -8) {
            cpu_used_value = -8;
        } else if (cpu_used_value > 8) {
            cpu_used_value = 8; // set to default (fastest) value
        }
    }

    rc = vpx_codec_control(vc->encoder, VP8E_SET_CPUUSED, cpu_used_value);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder VP8E_SET_CPUUSED setting: %s value=%d", vpx_codec_err_to_string(rc),
                     (int)cpu_used_value);
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    } else {
        LOGGER_WARNING(log, "set encoder VP8E_SET_CPUUSED setting: %s value=%d", vpx_codec_err_to_string(rc),
                       (int)cpu_used_value);
    }

    vc->video_encoder_cpu_used = cpu_used_value;
    vc->video_encoder_cpu_used_prev = cpu_used_value;

#ifdef VIDEO_CODEC_ENCODER_USE_FRAGMENTS

    if (vc->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_VP9) {
        rc = vpx_codec_control(vc->encoder, VP8E_SET_TOKEN_PARTITIONS, VIDEO_CODEC_FRAGMENT_VPX_NUMS);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(log, "Failed to set encoder token partitions: %s", vpx_codec_err_to_string(rc));
        }
    }

#endif

    /*
    VP9E_SET_TILE_COLUMNS

    Codec control function to set number of tile columns.

    In encoding and decoding, VP9 allows an input image frame be partitioned
    into separated vertical tile columns, which can be encoded or decoded independently.
    This enables easy implementation of parallel encoding and decoding. This control requests
    the encoder to use column tiles in encoding an input frame, with number of tile columns
    (in Log2 unit) as the parameter: 0 = 1 tile column 1 = 2 tile columns
    2 = 4 tile columns ..... n = 2**n tile columns The requested tile columns will
    be capped by encoder based on image size limitation (The minimum width of a
    tile column is 256 pixel, the maximum is 4096).

    By default, the value is 0, i.e. one single column tile for entire image.

    Supported in codecs: VP9
     */

    if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9) {
        rc = vpx_codec_control(vc->encoder, VP9E_SET_TILE_COLUMNS, VIDEO__VP9E_SET_TILE_COLUMNS);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
            vpx_codec_destroy(vc->encoder);
            goto BASE_CLEANUP_1;
        }

        rc = vpx_codec_control(vc->encoder, VP9E_SET_TILE_ROWS, VIDEO__VP9E_SET_TILE_ROWS);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
            vpx_codec_destroy(vc->encoder);
            goto BASE_CLEANUP_1;
        }
    }


#if 0

    if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9) {
        if (1 == 2) {
            rc = vpx_codec_control(vc->encoder, VP9E_SET_LOSSLESS, 1);

            LOGGER_WARNING(vc->log, "setting VP9 lossless video quality(2): ON");

            if (rc != VPX_CODEC_OK) {
                LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
                vpx_codec_destroy(vc->encoder);
                goto BASE_CLEANUP_1;
            }
        } else {
            rc = vpx_codec_control(vc->encoder, VP9E_SET_LOSSLESS, 0);

            LOGGER_WARNING(vc->log, "setting VP9 lossless video quality(2): OFF");

            if (rc != VPX_CODEC_OK) {
                LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
                vpx_codec_destroy(vc->encoder);
                goto BASE_CLEANUP_1;
            }
        }
    }

#endif


    /*
    VPX_CTRL_USE_TYPE(VP8E_SET_NOISE_SENSITIVITY,  unsigned int)
    control function to set noise sensitivity
      0: off, 1: OnYOnly, 2: OnYUV, 3: OnYUVAggressive, 4: Adaptive
    */
    /*
      rc = vpx_codec_control(vc->encoder, VP8E_SET_NOISE_SENSITIVITY, 2);

      if (rc != VPX_CODEC_OK) {
          LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
          vpx_codec_destroy(vc->encoder);
          goto BASE_CLEANUP_1;
      }
     */

    // VP8E_SET_STATIC_THRESHOLD









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
    vc->last_decoded_frame_ts = 0;
    vc->last_encoded_frame_ts = 0;
    vc->flag_end_video_fragment = 0;
    vc->last_seen_fragment_num = 0;
    vc->count_old_video_frames_seen = 0;
    vc->last_seen_fragment_seqnum = -1;
    vc->fragment_buf_counter = 0;

    for (int k = 0; k < VIDEO_DECODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES; k++) {
        vc->decoder_soft_deadline[k] = MAX_DECODE_TIME_US;
    }

    vc->decoder_soft_deadline_index = 0;

    for (int k = 0; k < VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES; k++) {
        vc->encoder_soft_deadline[k] = MAX_ENCODE_TIME_US;
    }

    vc->encoder_soft_deadline_index = 0;

    uint16_t jk = 0;

    for (jk = 0; jk < (uint16_t)VIDEO_MAX_FRAGMENT_BUFFER_COUNT; jk++) {
        vc->vpx_frames_buf_list[jk] = NULL;
    }

    return vc;

BASE_CLEANUP_1:
    vpx_codec_destroy(vc->decoder);
BASE_CLEANUP:
    pthread_mutex_destroy(vc->queue_mutex);
    rb_kill(vc->vbuf_raw);
    free(vc);
    return NULL;
}

VCSession *vc_new(Mono_Time *mono_time, const Logger *log, ToxAV *av, uint32_t friend_number,
                  toxav_video_receive_frame_cb *cb, void *cb_data)
{
    VCSession *vc = (VCSession *)calloc(sizeof(VCSession), 1);

    if (!vc) {
        LOGGER_WARNING(log, "Allocation failed! Application might misbehave!");
        return NULL;
    }

    if (create_recursive_mutex(vc->queue_mutex) != 0) {
        LOGGER_WARNING(log, "Failed to create recursive mutex!");
        free(vc);
        return NULL;
    }

    LOGGER_WARNING(log, "vc_new ...");

    // options ---
    vc->video_encoder_cpu_used = VP8E_SET_CPUUSED_VALUE;
    vc->video_encoder_cpu_used_prev = vc->video_encoder_cpu_used;
    vc->video_encoder_vp8_quality = TOXAV_ENCODER_VP8_QUALITY_NORMAL;
    vc->video_encoder_vp8_quality_prev = vc->video_encoder_vp8_quality;
    vc->video_rc_max_quantizer = TOXAV_ENCODER_VP8_RC_MAX_QUANTIZER_NORMAL;
    vc->video_rc_max_quantizer_prev = vc->video_rc_max_quantizer;
    vc->video_rc_min_quantizer = TOXAV_ENCODER_VP8_RC_MIN_QUANTIZER_NORMAL;
    vc->video_rc_min_quantizer_prev = vc->video_rc_min_quantizer;
    vc->video_encoder_coded_used = TOXAV_ENCODER_CODEC_USED_VP8; // DEFAULT: VP8 !!
    vc->video_encoder_frame_orientation_angle = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_0;
    vc->video_encoder_coded_used_prev = vc->video_encoder_coded_used;
#ifdef RASPBERRY_PI_OMX
    vc->video_encoder_coded_used_hw_accel = TOXAV_ENCODER_CODEC_HW_ACCEL_OMX_PI;
#else
    vc->video_encoder_coded_used_hw_accel = TOXAV_ENCODER_CODEC_HW_ACCEL_NONE;
#endif
    vc->video_keyframe_method = TOXAV_ENCODER_KF_METHOD_NORMAL;
    vc->video_keyframe_method_prev = vc->video_keyframe_method;
    vc->video_decoder_error_concealment = VIDEO__VP8_DECODER_ERROR_CONCEALMENT;
    vc->video_decoder_error_concealment_prev = vc->video_decoder_error_concealment;
    vc->video_decoder_codec_used = TOXAV_ENCODER_CODEC_USED_VP8; // DEFAULT: VP8 !!
    vc->send_keyframe_request_received = 0;
    vc->h264_video_capabilities_received = 0; // WARNING: always set to zero (0) !!
    vc->show_own_video = 0; // WARNING: always set to zero (0) !!
    vc->skip_fps = 0;
    vc->skip_fps_counter = 0;
    vc->skip_fps_release_counter = 0;
    vc->video_bitrate_autoset = 1;

    vc->dummy_ntp_local_start = 0;
    vc->dummy_ntp_local_end = 0;
    vc->dummy_ntp_remote_start = 0;
    vc->dummy_ntp_remote_end = 0;
    vc->rountrip_time_ms = 0;
    vc->video_play_delay = 0;
    vc->video_play_delay_real = 0;
    vc->video_frame_buffer_entries = 0;
    vc->parsed_h264_sps_profile_i = 0;
    vc->parsed_h264_sps_level_i = 0;
    vc->last_sent_keyframe_ts = 0;
    vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_0;

    vc->last_incoming_frame_ts = 0;
    vc->last_parsed_h264_sps_ts = 0;
    vc->timestamp_difference_to_sender = 0;
    vc->timestamp_difference_adjustment = -450;
    vc->tsb_range_ms = 60;
    vc->startup_video_timespan = 8000;
    vc->incoming_video_bitrate_last_changed = 0;
    vc->network_round_trip_time_last_cb_ts = 0;
    vc->incoming_video_bitrate_last_cb_ts = 0;
    vc->last_requested_lower_fps_ts = 0;
    vc->encoder_frame_has_record_timestamp = 1;
    vc->video_max_bitrate = VIDEO_BITRATE_MAX_AUTO_VALUE_H264;
    vc->video_decoder_buffer_ms = MIN_AV_BUFFERING_MS;
    vc->video_decoder_add_delay_ms = 0;
    vc->video_decoder_adjustment_base_ms = MIN_AV_BUFFERING_MS - AV_BUFFERING_DELTA_MS;
    vc->client_video_capture_delay_ms = 0;
    vc->remote_client_video_capture_delay_ms = 0;
    // options ---

    vc->incoming_video_frames_gap_ms_index = 0;
    vc->incoming_video_frames_gap_last_ts = 0;
    vc->incoming_video_frames_gap_ms_mean_value = 0;

    // set h264 callback
    vc->vcb_h264 = av->vcb_h264;
    vc->vcb_h264_user_data = av->vcb_h264_user_data;

    for (int i = 0; i < VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES; i++) {
        vc->incoming_video_frames_gap_ms[i] = 0;
    }

#ifdef USE_TS_BUFFER_FOR_VIDEO

    if (!(vc->vbuf_raw = tsb_new(VIDEO_RINGBUFFER_BUFFER_ELEMENTS))) {
        LOGGER_WARNING(log, "vc_new:rb_new FAILED");
        vc->vbuf_raw = NULL;
        goto BASE_CLEANUP;
    }

#else

    if (!(vc->vbuf_raw = rb_new(VIDEO_RINGBUFFER_BUFFER_ELEMENTS))) {
        LOGGER_WARNING(log, "vc_new:rb_new FAILED");
        vc->vbuf_raw = NULL;
        goto BASE_CLEANUP;
    }

#endif

    LOGGER_WARNING(log, "vc_new:rb_new OK");

    // HINT: tell client what encoder and decoder are in use now -----------
    if (av->call_comm_cb) {

        TOXAV_CALL_COMM_INFO cmi;
        cmi = TOXAV_CALL_COMM_DECODER_IN_USE_VP8;

        if (vc->video_decoder_codec_used == TOXAV_ENCODER_CODEC_USED_H264) {
            // don't the the friend if we have HW accel, since it would reveal HW and platform info
            cmi = TOXAV_CALL_COMM_DECODER_IN_USE_H264;
        }

        av->call_comm_cb(av, friend_number, cmi, 0, av->call_comm_cb_user_data);


        cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_VP8;

        if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_H264) {
            if (vc->video_encoder_coded_used_hw_accel == TOXAV_ENCODER_CODEC_HW_ACCEL_OMX_PI) {
                cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264_OMX_PI;
            } else {
                cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264;
            }
        }

        av->call_comm_cb(av, friend_number, cmi, 0, av->call_comm_cb_user_data);
    }

    // HINT: tell client what encoder and decoder are in use now -----------

    // HINT: initialize the H264 encoder


#ifdef RASPBERRY_PI_OMX
    LOGGER_WARNING(log, "OMX:002");
    vc = vc_new_h264_omx_raspi(log, av, friend_number, cb, cb_data, vc);
    LOGGER_WARNING(log, "OMX:003");
#else
    vc = vc_new_h264(log, av, friend_number, cb, cb_data, vc);
#endif

    // HINT: initialize VP8 encoder
    return vc_new_vpx(log, av, friend_number, cb, cb_data, vc);

BASE_CLEANUP:
    pthread_mutex_destroy(vc->queue_mutex);

#ifdef USE_TS_BUFFER_FOR_VIDEO
    tsb_drain((TSBuffer *)vc->vbuf_raw);
    tsb_kill((TSBuffer *)vc->vbuf_raw);
#else
    rb_kill((RingBuffer *)vc->vbuf_raw);
#endif
    vc->vbuf_raw = NULL;
    free(vc);
    return NULL;
}



void vc_kill(VCSession *vc)
{
    if (!vc) {
        return;
    }

#ifdef RASPBERRY_PI_OMX
    vc_kill_h264_omx_raspi(vc);
#else
    vc_kill_h264(vc);
#endif
    vc_kill_vpx(vc);

    void *p;
    uint64_t dummy;

#ifdef USE_TS_BUFFER_FOR_VIDEO
    tsb_drain((TSBuffer *)vc->vbuf_raw);
    tsb_kill((TSBuffer *)vc->vbuf_raw);
#else

    while (rb_read((RingBuffer *)vc->vbuf_raw, &p, &dummy)) {
        free(p);
    }

    rb_kill((RingBuffer *)vc->vbuf_raw);
#endif

    vc->vbuf_raw = NULL;

    pthread_mutex_destroy(vc->queue_mutex);

    LOGGER_DEBUG(vc->log, "Terminated video handler: %p", (void *)vc);
    free(vc);
}


void video_switch_decoder(VCSession *vc, TOXAV_ENCODER_CODEC_USED_VALUE decoder_to_use)
{
    if (vc->video_decoder_codec_used != (int32_t)decoder_to_use) {
        if ((decoder_to_use == TOXAV_ENCODER_CODEC_USED_VP8)
                || (decoder_to_use == TOXAV_ENCODER_CODEC_USED_VP9)
                || (decoder_to_use == TOXAV_ENCODER_CODEC_USED_H264)) {

            vc->video_decoder_codec_used = decoder_to_use;
            LOGGER_ERROR(vc->log, "**switching DECODER to **:%d",
                         (int)vc->video_decoder_codec_used);


            if (vc->av) {
                if (vc->av->call_comm_cb) {

                    TOXAV_CALL_COMM_INFO cmi;
                    cmi = TOXAV_CALL_COMM_DECODER_IN_USE_VP8;

                    if (vc->video_decoder_codec_used == TOXAV_ENCODER_CODEC_USED_H264) {
                        cmi = TOXAV_CALL_COMM_DECODER_IN_USE_H264;
                    }

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         cmi, 0, vc->av->call_comm_cb_user_data);

                }
            }


        }
    }
}


/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */
uint8_t vc_iterate(VCSession *vc, Messenger *m, uint8_t skip_video_flag, uint64_t *a_r_timestamp,
                   uint64_t *a_l_timestamp,
                   uint64_t *v_r_timestamp, uint64_t *v_l_timestamp, BWController *bwc,
                   int64_t *timestamp_difference_adjustment_,
                   int64_t *timestamp_difference_to_sender_)
{

    if (!vc) {
        return 0;
    }

    uint8_t ret_value = 0;
    struct RTPMessage *p;
    bool have_requested_index_frame = false;

    vpx_codec_err_t rc;

    pthread_mutex_lock(vc->queue_mutex);

    uint64_t frame_flags;
    uint8_t data_type;
    uint8_t h264_encoded_video_frame = 0;

    uint32_t full_data_len;

#ifdef USE_TS_BUFFER_FOR_VIDEO
    uint32_t timestamp_out_ = 0;
    uint32_t timestamp_min = 0;
    uint32_t timestamp_max = 0;

    *timestamp_difference_to_sender_ = vc->timestamp_difference_to_sender;

    tsb_get_range_in_buffer((TSBuffer *)vc->vbuf_raw, &timestamp_min, &timestamp_max);


    int64_t want_remote_video_ts = (current_time_monotonic(m->mono_time) + vc->timestamp_difference_to_sender +
                                    vc->timestamp_difference_adjustment);

    uint32_t timestamp_want_get = (uint32_t)want_remote_video_ts;


    // HINT: compensate for older clients ----------------
    if (vc->encoder_frame_has_record_timestamp == 0) {
        LOGGER_DEBUG(vc->log, "old client:002");
        vc->tsb_range_ms = (UINT32_MAX - 1);
        timestamp_want_get = (UINT32_MAX - 1);
        vc->startup_video_timespan = 0;
    }

    // HINT: compensate for older clients ----------------

#if 0

    if ((int)tsb_size((TSBuffer *)vc->vbuf_raw) > 0) {
        LOGGER_ERROR(vc->log, "FC:%d min=%ld max=%ld want=%d diff=%d adj=%d roundtrip=%d",
                     (int)tsb_size((TSBuffer *)vc->vbuf_raw),
                     timestamp_min,
                     timestamp_max,
                     (int)timestamp_want_get,
                     (int)timestamp_want_get - (int)timestamp_max,
                     (int)vc->timestamp_difference_adjustment,
                     (int)vc->rountrip_time_ms);
    }

#endif

    // HINT: correct for very false values ------------
    if ((int)tsb_size((TSBuffer *)vc->vbuf_raw) > (VIDEO_RINGBUFFER_BUFFER_ELEMENTS - 2)) {
        // HINT: buffer with incoming video frames is very full
        if (timestamp_want_get > (timestamp_max + 100)) {
            // we wont get a frame like this
            vc->timestamp_difference_adjustment = vc->timestamp_difference_adjustment -
                                                  (timestamp_want_get - timestamp_max) - 10;

            LOGGER_ERROR(vc->log, "DEFF_CORR:--:%d", (int)((timestamp_want_get - timestamp_max) - 10));

            want_remote_video_ts = (current_time_monotonic(m->mono_time) + vc->timestamp_difference_to_sender +
                                    vc->timestamp_difference_adjustment);

            timestamp_want_get = (uint32_t)want_remote_video_ts;
        } else if ((timestamp_want_get + 100) < timestamp_min) {
            vc->timestamp_difference_adjustment = vc->timestamp_difference_adjustment +
                                                  (timestamp_min - timestamp_want_get) + 10;

            LOGGER_ERROR(vc->log, "DEFF_CORR:++++:%d", (int)((timestamp_min - timestamp_want_get) + 10));

            want_remote_video_ts = (current_time_monotonic(m->mono_time) + vc->timestamp_difference_to_sender +
                                    vc->timestamp_difference_adjustment);

            timestamp_want_get = (uint32_t)want_remote_video_ts;
        }
    }


    // HINT: correct for very false values ------------

    uint16_t removed_entries;
    uint16_t is_skipping = 0;

    // HINT: give me video frames that happend "now" minus some diff
    //       get a videoframe for timestamp [timestamp_want_get + (uint32_t)(video_decoder_add_delay_ms)]
    if (tsb_read((TSBuffer *)vc->vbuf_raw, vc->log, (void **)&p, &frame_flags,
                 &timestamp_out_,
                 timestamp_want_get + (uint32_t)(vc->video_decoder_add_delay_ms),
                 vc->tsb_range_ms + vc->startup_video_timespan,
                 &removed_entries,
                 &is_skipping)) {
#else

    if (rb_read((RingBuffer *)vc->vbuf_raw, (void **)&p, &frame_flags)) {
#endif



#if 1

        if ((is_skipping > 0) && (removed_entries > 0)) {
            if ((vc->last_requested_lower_fps_ts + 10000) < current_time_monotonic(m->mono_time)) {


                // HINT: tell sender to turn down video FPS -------------
                uint32_t pkg_buf_len = 3;
                uint8_t pkg_buf[pkg_buf_len];
                pkg_buf[0] = PACKET_TOXAV_COMM_CHANNEL;
                pkg_buf[1] = PACKET_TOXAV_COMM_CHANNEL_LESS_VIDEO_FPS;

#if 0

                if ((vc->last_requested_lower_fps_ts + 12000) < current_time_monotonic(m->mono_time)) {
                    pkg_buf[2] = 2;
                } else {
                    pkg_buf[2] = 3; // skip every 3rd video frame and dont encode and dont sent it
                }

#else
                pkg_buf[2] = 3; // skip every 3rd video frame and dont encode and dont sent it
#endif

                int result = send_custom_lossless_packet(vc->av->m, vc->friend_number, pkg_buf, pkg_buf_len);
                // HINT: tell sender to turn down video FPS -------------

                vc->last_requested_lower_fps_ts = current_time_monotonic(m->mono_time);

                LOGGER_DEBUG(vc->log, "request lower FPS from sender: %d ms : skip every %d", (int)is_skipping, (int)pkg_buf[2]);
            }
        }

#endif




#if 1
        LOGGER_DEBUG(vc->log, "rtt:drift:1:%d %d %d", (int)(vc->rountrip_time_ms),
                     (int)(-vc->timestamp_difference_adjustment),
                     (int)vc->video_decoder_adjustment_base_ms);

        if (vc->rountrip_time_ms > (-vc->timestamp_difference_adjustment - vc->video_decoder_adjustment_base_ms)) {
            // drift
            LOGGER_DEBUG(vc->log, "rtt:drift:2:%d > %d", (int)(vc->rountrip_time_ms),
                         (int)(-vc->timestamp_difference_adjustment - vc->video_decoder_adjustment_base_ms));

            LOGGER_DEBUG(vc->log, "rtt:drift:3:%d < %d", (int)(vc->timestamp_difference_adjustment),
                         (int)(-vc->video_decoder_buffer_ms));

            if (tsb_size((TSBuffer *)vc->vbuf_raw) < 10) {
                vc->timestamp_difference_adjustment = vc->timestamp_difference_adjustment - 1;
                LOGGER_DEBUG(vc->log, "rtt:drift:4:---1:%d", (int)(vc->timestamp_difference_adjustment));
            } else {
                vc->timestamp_difference_adjustment = vc->timestamp_difference_adjustment + 1;
            }
        } else if (vc->rountrip_time_ms < (-vc->timestamp_difference_adjustment - vc->video_decoder_adjustment_base_ms)) {
            // drift
            LOGGER_DEBUG(vc->log, "rtt:drift:5:%d << %d", (int)(vc->rountrip_time_ms),
                         (int)(-vc->timestamp_difference_adjustment - vc->video_decoder_adjustment_base_ms));

            if (vc->timestamp_difference_adjustment <= -vc->video_decoder_buffer_ms) {
                LOGGER_DEBUG(vc->log, "rtt:drift:6:%d < %d", (int)(vc->timestamp_difference_adjustment),
                             (int)(-vc->video_decoder_buffer_ms));

                vc->timestamp_difference_adjustment = vc->timestamp_difference_adjustment + 1;
                LOGGER_DEBUG(vc->log, "rtt:drift:7:+1:%d", (int)(vc->timestamp_difference_adjustment));
            }
        }

#endif




        LOGGER_DEBUG(vc->log, "XLS01:%d,%d",
                     (int)(timestamp_want_get - current_time_monotonic(m->mono_time)),
                     (int)(timestamp_out_ - current_time_monotonic(m->mono_time))
                    );

        const struct RTPHeader *header_v3_0 = (void *) & (p->header);

        vc->video_play_delay = ((current_time_monotonic(m->mono_time) + vc->timestamp_difference_to_sender) - timestamp_out_);
        vc->video_play_delay_real = vc->video_play_delay;

        vc->video_frame_buffer_entries = (uint32_t)tsb_size((TSBuffer *)vc->vbuf_raw);

        LOGGER_DEBUG(vc->log,
                     "seq:%d FC:%d min=%d max=%d want=%d got=%d diff=%d rm=%d pdelay=%d pdelayr=%d adj=%d dts=%d rtt=%d",
                     (int)header_v3_0->sequnum,
                     (int)tsb_size((TSBuffer *)vc->vbuf_raw),
                     timestamp_min,
                     timestamp_max,
                     (int)timestamp_want_get,
                     (int)timestamp_out_,
                     ((int)timestamp_want_get - (int)timestamp_out_),
                     (int)removed_entries,
                     (int)vc->video_play_delay,
                     (int)vc->video_play_delay_real,
                     (int)vc->timestamp_difference_adjustment,
                     (int)vc->timestamp_difference_to_sender,
                     (int)vc->rountrip_time_ms);

        uint16_t buf_size = tsb_size((TSBuffer *)vc->vbuf_raw);
        int32_t diff_want_to_got = (int)timestamp_want_get - (int)timestamp_out_;


        LOGGER_DEBUG(vc->log, "values:diff_to_sender=%d adj=%d tsb_range=%d bufsize=%d",
                     (int)vc->timestamp_difference_to_sender, (int)vc->timestamp_difference_adjustment,
                     (int)vc->tsb_range_ms,
                     (int)buf_size);


        if (vc->startup_video_timespan > 0) {
            vc->startup_video_timespan = 0;
        }



        // TODO: make it available to the audio session
        // bad hack -> make better!
        *timestamp_difference_adjustment_ = vc->timestamp_difference_adjustment;


        LOGGER_DEBUG(vc->log, "--VSEQ:%d", (int)header_v3_0->sequnum);

        data_type = (uint8_t)((frame_flags & RTP_KEY_FRAME) != 0);
        h264_encoded_video_frame = (uint8_t)((frame_flags & RTP_ENCODER_IS_H264) != 0);

        uint8_t video_orientation_bit0 = (uint8_t)((frame_flags & RTP_ENCODER_VIDEO_ROTATION_ANGLE_BIT0) != 0);
        uint8_t video_orientation_bit1 = (uint8_t)((frame_flags & RTP_ENCODER_VIDEO_ROTATION_ANGLE_BIT1) != 0);

        // LOGGER_WARNING(vc->log, "FRAMEFLAGS:%d", (int)frame_flags);

        if ((video_orientation_bit0 == 0) && (video_orientation_bit1 == 0)) {
            vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_0;
        } else if ((video_orientation_bit0 == 1) && (video_orientation_bit1 == 0)) {
            vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_90;
        } else if ((video_orientation_bit0 == 0) && (video_orientation_bit1 == 1)) {
            vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_180;
        } else if ((video_orientation_bit0 == 1) && (video_orientation_bit1 == 1)) {
            vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_270;
        }

        bwc_add_recv(bwc, header_v3_0->data_length_full);

        if ((int32_t)header_v3_0->sequnum < (int32_t)vc->last_seen_fragment_seqnum) {
            // drop frame with too old sequence number
            LOGGER_WARNING(vc->log, "skipping incoming video frame (0) with sn=%d lastseen=%d old_frames_count=%d",
                           (int)header_v3_0->sequnum,
                           (int)vc->last_seen_fragment_seqnum,
                           (int)vc->count_old_video_frames_seen);

            vc->count_old_video_frames_seen++;

            if ((int32_t)(header_v3_0->sequnum + 1) != (int32_t)vc->last_seen_fragment_seqnum) {
                // TODO: check why we often get exactly the previous video frame here?!?!
            }

            if (vc->count_old_video_frames_seen > 6) {
                // if we see more than 6 old video frames in a row, then either there was
                // a seqnum rollover or something else. just play those frames then
                vc->last_seen_fragment_seqnum = (int32_t)header_v3_0->sequnum;
                vc->count_old_video_frames_seen = 0;
            }

            free(p);
            pthread_mutex_unlock(vc->queue_mutex);
            return 0;
        }

        if ((int32_t)header_v3_0->sequnum != (int32_t)(vc->last_seen_fragment_seqnum + 1)) {
            int32_t missing_frames_count = (int32_t)header_v3_0->sequnum -
                                           (int32_t)(vc->last_seen_fragment_seqnum + 1);


            const Messenger *mm = (Messenger *)(vc->av->m);
            const Messenger_Options *mo = (Messenger_Options *) & (mm->options);

#define NORMAL_MISSING_FRAME_COUNT_TOLERANCE 0
#define WHEN_SKIPPING_MISSING_FRAME_COUNT_TOLERANCE 2

            int32_t missing_frame_tolerance = NORMAL_MISSING_FRAME_COUNT_TOLERANCE;

            if (is_skipping > 0) {
                // HINT: workaround, if we are skipping frames because client is too slow
                //       we assume the missing frames here are the skipped ones
                missing_frame_tolerance = WHEN_SKIPPING_MISSING_FRAME_COUNT_TOLERANCE;
            }

            if (missing_frames_count > missing_frame_tolerance) {

                // HINT: if whole video frames are missing here, they most likely have been
                //       kicked out of the ringbuffer because the sender is sending at too much FPS
                //       which out client cant handle. so in the future signal sender to send less FPS!

#ifndef RPIZEROW
                LOGGER_WARNING(vc->log, "missing? sn=%d lastseen=%d",
                               (int)header_v3_0->sequnum,
                               (int)vc->last_seen_fragment_seqnum);


                LOGGER_DEBUG(vc->log, "missing %d video frames (m1)", (int)missing_frames_count);
#endif

                if (vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264) {
                    rc = vpx_codec_decode(vc->decoder, NULL, 0, NULL, VPX_DL_REALTIME);
                }

                // HINT: give feedback that we lost some bytes (based on the size of this frame)
                bwc_add_lost_v3(bwc, (uint32_t)(header_v3_0->data_length_full * missing_frames_count), true);
#ifndef RPIZEROW
                LOGGER_ERROR(vc->log, "BWC:lost:002:missing count=%d", (int)missing_frames_count);
#endif
            }
        }


        // TODO: check for seqnum rollover!!
        vc->count_old_video_frames_seen = 0;
        vc->last_seen_fragment_seqnum = header_v3_0->sequnum;

        if (skip_video_flag == 1) {
#if 1

            if ((int)data_type != (int)video_frame_type_KEYFRAME) {
                free(p);
                LOGGER_ERROR(vc->log, "skipping incoming video frame (1)");

                if (vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264) {
                    rc = vpx_codec_decode(vc->decoder, NULL, 0, NULL, VPX_DL_REALTIME);
                }

                // HINT: give feedback that we lost some bytes (based on the size of this frame)
                bwc_add_lost_v3(bwc, header_v3_0->data_length_full, false);
                LOGGER_ERROR(vc->log, "BWC:lost:003");

                pthread_mutex_unlock(vc->queue_mutex);
                return 0;
            }

#endif
        } else {
            // NOOP
        }

        pthread_mutex_unlock(vc->queue_mutex);

        const struct RTPHeader *header_v3 = (void *) & (p->header);

        if (header_v3->flags & RTP_LARGE_FRAME) {
            full_data_len = header_v3->data_length_full;
            LOGGER_DEBUG(vc->log, "vc_iterate:001:full_data_len=%d", (int)full_data_len);
        } else {
            full_data_len = p->len;
            LOGGER_DEBUG(vc->log, "vc_iterate:002");
        }

        // LOGGER_DEBUG(vc->log, "vc_iterate: rb_read p->len=%d data_type=%d", (int)full_data_len, (int)data_type);
        // LOGGER_DEBUG(vc->log, "vc_iterate: rb_read rb size=%d", (int)rb_size((RingBuffer *)vc->vbuf_raw));



        // HINT: give feedback that we lost some bytes
        if (header_v3->received_length_full < full_data_len) {
            // const Messenger *mm = (Messenger *)(vc->av->m);
            // const Messenger_Options *mo = (Messenger_Options *) & (mm->options);

            bwc_add_lost_v3(bwc, (full_data_len - header_v3->received_length_full), false);

            float percent_lost = 0.0f;

            if (header_v3->received_length_full > 0) {
                percent_lost = (float)full_data_len / (float)header_v3->received_length_full;
            }

            LOGGER_ERROR(vc->log, "BWC:lost:004:lost bytes=%d recevied=%d full=%d per=%.3f",
                         (int)(full_data_len - header_v3->received_length_full),
                         (int)header_v3->received_length_full,
                         (int)full_data_len,
                         (float)percent_lost);
        }


        if ((int)data_type == (int)video_frame_type_KEYFRAME) {

            int percent_recvd = 100;

            if (full_data_len > 0) {
                percent_recvd = (int)(((float)header_v3->received_length_full / (float)full_data_len) * 100.0f);
            }

#if 0

            if (percent_recvd < 100) {
                LOGGER_DEBUG(vc->log, "RTP_RECV:sn=%ld fn=%ld pct=%d%% *I* len=%ld recv_len=%ld",
                             (long)header_v3->sequnum,
                             (long)header_v3->fragment_num,
                             percent_recvd,
                             (long)full_data_len,
                             (long)header_v3->received_length_full);
            } else {
                LOGGER_DEBUG(vc->log, "RTP_RECV:sn=%ld fn=%ld pct=%d%% *I* len=%ld recv_len=%ld",
                             (long)header_v3->sequnum,
                             (long)header_v3->fragment_num,
                             percent_recvd,
                             (long)full_data_len,
                             (long)header_v3->received_length_full);
            }

#endif

#if 0

            if ((percent_recvd < 100) && (have_requested_index_frame == false)) {
                if ((vc->last_requested_keyframe_ts + VIDEO_MIN_REQUEST_KEYFRAME_INTERVAL_MS_FOR_KF)
                        < current_time_monotonic(m->mono_time)) {
                    // if keyframe received has less than 100% of the data, request a new keyframe
                    // from the sender
                    uint32_t pkg_buf_len = 2;
                    uint8_t pkg_buf[pkg_buf_len];
                    pkg_buf[0] = PACKET_TOXAV_COMM_CHANNEL;
                    pkg_buf[1] = PACKET_TOXAV_COMM_CHANNEL_REQUEST_KEYFRAME;

                    if (-1 == send_custom_lossless_packet(m, vc->friend_number, pkg_buf, pkg_buf_len)) {
                        LOGGER_WARNING(vc->log,
                                       "PACKET_TOXAV_COMM_CHANNEL_REQUEST_KEYFRAME:RTP send failed");
                    } else {
                        LOGGER_WARNING(vc->log,
                                       "PACKET_TOXAV_COMM_CHANNEL_REQUEST_KEYFRAME:RTP Sent.");
                        vc->last_requested_keyframe_ts = current_time_monotonic(m->mono_time);
                    }
                }
            }

#endif

        } else {
            LOGGER_DEBUG(vc->log, "RTP_RECV:sn=%ld fn=%ld pct=%d%% len=%ld recv_len=%ld",
                         (long)header_v3->sequnum,
                         (long)header_v3->fragment_num,
                         (int)(((float)header_v3->received_length_full / (float)full_data_len) * 100.0f),
                         (long)full_data_len,
                         (long)header_v3->received_length_full);
        }


        // LOGGER_ERROR(vc->log, "h264_encoded_video_frame=%d vc->video_decoder_codec_used=%d",
        //             (int)h264_encoded_video_frame,
        //             (int)vc->video_decoder_codec_used);

        if (DISABLE_H264_DECODER_FEATURE == 0) {

            if ((vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264)
                    && (h264_encoded_video_frame == 1)) {
                LOGGER_ERROR(vc->log, "h264_encoded_video_frame:AA");
                video_switch_decoder(vc, TOXAV_ENCODER_CODEC_USED_H264);

            } else if ((vc->video_decoder_codec_used == TOXAV_ENCODER_CODEC_USED_H264)
                       && (h264_encoded_video_frame == 0)) {
                LOGGER_ERROR(vc->log, "h264_encoded_video_frame:BB");
                // HINT: once we switched to H264 never switch back to VP8 until this call ends
                // video_switch_decoder(vc, TOXAV_ENCODER_CODEC_USED_VP8);
            }
        }

        // HINT: somtimes the singaling of H264 capability does not work
        //       as workaround send it again on the first 30 frames

        if (DISABLE_H264_DECODER_FEATURE != 1) {
            if ((vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264)
                    && ((long)header_v3->sequnum < 30)) {

                // HINT: tell friend that we have H264 decoder capabilities (3) -------
                uint32_t pkg_buf_len = 2;
                uint8_t pkg_buf[pkg_buf_len];
                pkg_buf[0] = PACKET_TOXAV_COMM_CHANNEL;
                pkg_buf[1] = PACKET_TOXAV_COMM_CHANNEL_HAVE_H264_VIDEO;

                int result = send_custom_lossless_packet(m, vc->friend_number, pkg_buf, pkg_buf_len);
                LOGGER_ERROR(vc->log, "PACKET_TOXAV_COMM_CHANNEL_HAVE_H264_VIDEO=%d\n", (int)result);
                // HINT: tell friend that we have H264 decoder capabilities -------

            }
        }

        if (vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264) {
            // LOGGER_ERROR(vc->log, "DEC:VP8------------");
            decode_frame_vpx(vc, m, skip_video_flag, a_r_timestamp,
                             a_l_timestamp,
                             v_r_timestamp, v_l_timestamp,
                             header_v3, p,
                             rc, full_data_len,
                             &ret_value);
        } else {
            // LOGGER_ERROR(vc->log, "DEC:H264------------");
#ifdef RASPBERRY_PI_OMX
            decode_frame_h264_omx_raspi(vc, m, skip_video_flag, a_r_timestamp,
                                        a_l_timestamp,
                                        v_r_timestamp, v_l_timestamp,
                                        header_v3, p,
                                        rc, full_data_len,
                                        &ret_value);
#else

#ifdef DEBUG_SHOW_H264_DECODING_TIME
            uint32_t start_time_ms = current_time_monotonic(m->mono_time);
#endif
            decode_frame_h264(vc, m, skip_video_flag, a_r_timestamp,
                              a_l_timestamp,
                              v_r_timestamp, v_l_timestamp,
                              header_v3, p,
                              rc, full_data_len,
                              &ret_value);

#ifdef DEBUG_SHOW_H264_DECODING_TIME
            uint32_t end_time_ms = current_time_monotonic(m->mono_time);

            if ((int)(end_time_ms - start_time_ms) > 4) {
                LOGGER_WARNING(vc->log, "decode_frame_h264: %d ms", (int)(end_time_ms - start_time_ms));
            }

#endif

#endif
        }

        return ret_value;
    } else {
        // no frame data available
        // LOGGER_WARNING(vc->log, "Error decoding video: rb_read");
        if (removed_entries > 0) {
            LOGGER_WARNING(vc->log, "removed entries=%d", (int)removed_entries);
        }
    }

    pthread_mutex_unlock(vc->queue_mutex);

    return ret_value;
}

/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */


int vc_queue_message(Mono_Time *mono_time, void *vcp, struct RTPMessage *msg)
{
    /* This function is called with complete messages
     * they have already been assembled. but not yet decoded
     * (data is still compressed by video codec)
     * this function gets called from handle_rtp_packet()
     */
    if (!vcp || !msg) {
        if (msg) {
            free(msg);
        }

        return -1;
    }

    VCSession *vc = (VCSession *)vcp;

    const struct RTPHeader *header_v3 = (void *) & (msg->header);
    const struct RTPHeader *header = &msg->header;

    if (msg->header.pt == (RTP_TYPE_VIDEO + 2) % 128) {
        LOGGER_WARNING(vc->log, "Got dummy!");
        free(msg);
        return 0;
    }

    if (msg->header.pt != RTP_TYPE_VIDEO % 128) {
        LOGGER_WARNING(vc->log, "Invalid payload type! pt=%d", (int)msg->header.pt);
        free(msg);
        return -1;
    }

    // calculate mean "frame incoming every x milliseconds" --------------
    if (vc->incoming_video_frames_gap_last_ts > 0) {
        uint32_t curent_gap = current_time_monotonic(mono_time) - vc->incoming_video_frames_gap_last_ts;

        vc->incoming_video_frames_gap_ms[vc->incoming_video_frames_gap_ms_index] = curent_gap;
        vc->incoming_video_frames_gap_ms_index = (vc->incoming_video_frames_gap_ms_index + 1) %
                VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES;

        uint32_t mean_value = 0;

        for (int k = 0; k < VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES; k++) {
            mean_value = mean_value + vc->incoming_video_frames_gap_ms[k];
        }

        if (mean_value == 0) {
            vc->incoming_video_frames_gap_ms_mean_value = 0;
        } else {
            vc->incoming_video_frames_gap_ms_mean_value = (mean_value * 10) / (VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES * 10);
        }

#if 0
        LOGGER_DEBUG(vc->log, "FPS:INCOMING=%d ms = %.1f fps mean=%d m=%d",
                     (int)curent_gap,
                     (float)(1000.0f / (curent_gap + 0.00001)),
                     (int)vc->incoming_video_frames_gap_ms_mean_value,
                     (int)mean_value);
#endif
    }

    vc->incoming_video_frames_gap_last_ts = current_time_monotonic(mono_time);
    // calculate mean "frame incoming every x milliseconds" --------------

    pthread_mutex_lock(vc->queue_mutex);

    LOGGER_DEBUG(vc->log, "TT:queue:V:fragnum=%ld", (long)header_v3->fragment_num);

    // older clients do not send the frame record timestamp
    // compensate by using the frame sennt timestamp
    if (msg->header.frame_record_timestamp == 0) {
        LOGGER_DEBUG(vc->log, "old client:001");
        msg->header.frame_record_timestamp = msg->header.timestamp;
    }


    if ((header->flags & RTP_LARGE_FRAME) && header->pt == RTP_TYPE_VIDEO % 128) {


        vc->last_incoming_frame_ts = header_v3->frame_record_timestamp;


        // give COMM data to client -------

        if ((vc->network_round_trip_time_last_cb_ts + 2000) < current_time_monotonic(mono_time)) {
            if (vc->av) {
                if (vc->av->call_comm_cb) {
                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_NETWORK_ROUND_TRIP_MS,
                                         (int64_t)vc->rountrip_time_ms,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_PLAY_DELAY,
                                         (int64_t)vc->video_play_delay_real,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_REMOTE_RECORD_DELAY,
                                         (int64_t)vc->remote_client_video_capture_delay_ms,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_PLAY_BUFFER_ENTRIES,
                                         (int64_t)vc->video_frame_buffer_entries,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_DECODER_H264_PROFILE,
                                         (int64_t)vc->parsed_h264_sps_profile_i,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_DECODER_H264_LEVEL,
                                         (int64_t)vc->parsed_h264_sps_level_i,
                                         vc->av->call_comm_cb_user_data);

                    vc->av->call_comm_cb(vc->av, vc->friend_number,
                                         TOXAV_CALL_COMM_PLAY_VIDEO_ORIENTATION,
                                         (int64_t)vc->video_incoming_frame_orientation,
                                         vc->av->call_comm_cb_user_data);

                    if (vc->incoming_video_frames_gap_ms_mean_value == 0) {
                        vc->av->call_comm_cb(vc->av, vc->friend_number,
                                             TOXAV_CALL_COMM_INCOMING_FPS,
                                             (int64_t)(9999),
                                             vc->av->call_comm_cb_user_data);
                    } else {
                        vc->av->call_comm_cb(vc->av, vc->friend_number,
                                             TOXAV_CALL_COMM_INCOMING_FPS,
                                             (int64_t)(1000 / vc->incoming_video_frames_gap_ms_mean_value),
                                             vc->av->call_comm_cb_user_data);
                    }
                }

            }

            vc->network_round_trip_time_last_cb_ts = current_time_monotonic(mono_time);
        }

        // give COMM data to client -------


        if (vc->show_own_video == 0) {


            if ((vc->incoming_video_bitrate_last_cb_ts + 2000) < current_time_monotonic(mono_time)) {
                if (vc->incoming_video_bitrate_last_changed != header->encoder_bit_rate_used) {
                    if (vc->av) {
                        if (vc->av->call_comm_cb) {
                            vc->av->call_comm_cb(vc->av, vc->friend_number,
                                                 TOXAV_CALL_COMM_DECODER_CURRENT_BITRATE,
                                                 (int64_t)header->encoder_bit_rate_used,
                                                 vc->av->call_comm_cb_user_data);
                        }
                    }

                    vc->incoming_video_bitrate_last_changed = header->encoder_bit_rate_used;
                }

                vc->incoming_video_bitrate_last_cb_ts = current_time_monotonic(mono_time);
            }


#ifdef USE_TS_BUFFER_FOR_VIDEO
            struct RTPMessage *msg_old = tsb_write((TSBuffer *)vc->vbuf_raw, msg,
                                                   (uint64_t)header->flags,
                                                   (uint32_t)header->frame_record_timestamp);
#else
            struct RTPMessage *msg_old = rb_write((RingBuffer *)vc->vbuf_raw, msg, (uint64_t)header->flags);
#endif

            if (msg_old) {

                // HINT: tell sender to turn down video FPS -------------
#if 0
                uint32_t pkg_buf_len = 3;
                uint8_t pkg_buf[pkg_buf_len];
                pkg_buf[0] = PACKET_TOXAV_COMM_CHANNEL;
                pkg_buf[1] = PACKET_TOXAV_COMM_CHANNEL_LESS_VIDEO_FPS;
                pkg_buf[2] = 3; // skip every 3rd video frame and dont encode and dont sent it

                int result = send_custom_lossless_packet(vc->av->m, vc->friend_number, pkg_buf, pkg_buf_len);
                // HINT: tell sender to turn down video FPS -------------
#endif
                LOGGER_DEBUG(vc->log, "FPATH:%d kicked out", (int)msg_old->header.sequnum);

                free(msg_old);
            }
        } else {
            // discard incoming frame, we want to see our outgoing frames instead
            if (msg) {
                free(msg);
            }
        }
    } else {
#ifdef USE_TS_BUFFER_FOR_VIDEO
        free(tsb_write((TSBuffer *)vc->vbuf_raw, msg, 0, current_time_monotonic(mono_time)));
#else
        free(rb_write((RingBuffer *)vc->vbuf_raw, msg, 0));
#endif
    }


    /* Calculate time since we received the last video frame */
    // use 5ms less than the actual time, to give some free room
    uint32_t t_lcfd = (current_time_monotonic(mono_time) - vc->linfts) - 5;
    vc->lcfd = t_lcfd > 100 ? vc->lcfd : t_lcfd;

#ifdef VIDEO_DECODER_SOFT_DEADLINE_AUTOTUNE

    // Autotune decoder softdeadline here ----------
    if (vc->last_decoded_frame_ts > 0) {
        long decode_time_auto_tune = (current_time_monotonic(mono_time) - vc->last_decoded_frame_ts) * 1000;

        if (decode_time_auto_tune == 0) {
            decode_time_auto_tune = 1; // 0 means infinite long softdeadline!
        }

        vc->decoder_soft_deadline[vc->decoder_soft_deadline_index] = decode_time_auto_tune;
        vc->decoder_soft_deadline_index = (vc->decoder_soft_deadline_index + 1) % VIDEO_DECODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES;

#if 0
        LOGGER_DEBUG(vc->log, "AUTOTUNE:INCOMING=%ld us = %.1f fps", (long)decode_time_auto_tune,
                     (float)(1000000.0f / decode_time_auto_tune));
#endif

    }

    vc->last_decoded_frame_ts = current_time_monotonic(mono_time);
    // Autotune decoder softdeadline here ----------
#endif

    vc->linfts = current_time_monotonic(mono_time);

    pthread_mutex_unlock(vc->queue_mutex);

    return 0;
}



int vc_reconfigure_encoder(Logger *log, VCSession *vc, uint32_t bit_rate, uint16_t width, uint16_t height,
                           int16_t kf_max_dist)
{
    if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8) {
        return vc_reconfigure_encoder_vpx(log, vc, bit_rate, width, height, kf_max_dist);
    } else {
#ifdef RASPBERRY_PI_OMX
        return vc_reconfigure_encoder_h264_omx_raspi(log, vc, bit_rate, width, height, kf_max_dist);
#else
        return vc_reconfigure_encoder_h264(log, vc, bit_rate, width, height, kf_max_dist);
#endif
    }
}


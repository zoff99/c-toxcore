/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
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

#include <assert.h>
#include <stdlib.h>

/*
  Soft deadline the decoder should attempt to meet, in us (microseconds). Set to zero for unlimited.
  By convention, the value 1 is used to mean "return as fast as possible."
*/
#define MAX_DECODE_TIME_US VPX_DL_REALTIME
/*
VPX_DL_REALTIME       (1)
deadline parameter analogous to VPx REALTIME mode.

VPX_DL_GOOD_QUALITY   (1000000)
deadline parameter analogous to VPx GOOD QUALITY mode.

VPX_DL_BEST_QUALITY   (0)
deadline parameter analogous to VPx BEST QUALITY mode.
*/

#define VP8E_SET_CPUUSED_VALUE (16)
/*
Codec control function to set encoder internal speed settings.
Changes in this value influences, among others, the encoder's selection of motion estimation methods.
Values greater than 0 will increase encoder speed at the expense of quality.

Note
    Valid range for VP8: -16..16 
    Valid range for VP9: -8..8
 */

/*
VP8E_SET_CQ_LEVEL 	

Codec control function to set constrained quality level.
Attention
    For this value to be used vpx_codec_enc_cfg_t::g_usage must be set to VPX_CQ. 
Note
    Valid range: 0..63
Supported in codecs: VP8, VP9 
 */

#define VIDEO_DECODE_BUFFER_SIZE (500) // ORIG VALUE: 20
#define VIDEO_BITRATE_INITIAL_VALUE 2500 // initialize encoder with this value. Target bandwidth to use for this stream, in kilobits per second.




// ---------- dirty hack ----------
// ---------- dirty hack ----------
// ---------- dirty hack ----------
int global__MAX_DECODE_TIME_US = MAX_DECODE_TIME_US;
int global__VP8E_SET_CPUUSED_VALUE = VP8E_SET_CPUUSED_VALUE;
int global__VPX_END_USAGE = VPX_VBR;
int global__VPX_KF_MAX_DIST = 12;
int global__VPX_G_LAG_IN_FRAMES = 0;
extern int global__MAX_ENCODE_TIME_US;

int global__VP8E_SET_CPUUSED_VALUE__prev_value = VP8E_SET_CPUUSED_VALUE;
int global__VPX_END_USAGE__prev_value = VPX_VBR;
int global__VPX_KF_MAX_DIST__prev_value = 12;
int global__VPX_G_LAG_IN_FRAMES__prev_value = 0;
extern int global__MAX_ENCODE_TIME_US__prev_value;
// ---------- dirty hack ----------
// ---------- dirty hack ----------
// ---------- dirty hack ----------


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

    /*
    VPX_CODEC_USE_FRAME_THREADING
       Enable frame-based multi-threading

    VPX_CODEC_USE_ERROR_CONCEALMENT
       Conceal errors in decoded frames
    */
    vpx_codec_dec_cfg_t  dec_cfg;
    dec_cfg.threads = 4; // Maximum number of threads to use
    dec_cfg.w = 800;
    dec_cfg.h = 600;
    rc = vpx_codec_dec_init(vc->decoder, VIDEO_CODEC_DECODER_INTERFACE, &dec_cfg, VPX_CODEC_USE_FRAME_THREADING);

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

    cfg.rc_target_bitrate = VIDEO_BITRATE_INITIAL_VALUE; /* Target bandwidth to use for this stream, in kilobits per second */
    cfg.g_w = 800;
    cfg.g_h = 600;
    cfg.g_pass = VPX_RC_ONE_PASS;

    /* zoff (in 2017) */
    cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT | VPX_ERROR_RESILIENT_PARTITIONS;
    cfg.g_lag_in_frames = global__VPX_G_LAG_IN_FRAMES;
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
    cfg.kf_min_dist = 0;
    cfg.kf_mode = VPX_KF_AUTO; // Encoder determines optimal placement automatically
    cfg.rc_end_usage = global__VPX_END_USAGE; // what quality mode?
    /*
     VPX_VBR 	Variable Bit Rate (VBR) mode
     VPX_CBR 	Constant Bit Rate (CBR) mode
     VPX_CQ 	Constrained Quality (CQ) mode -> give codec a hint that we may be on low bandwidth connection
     VPX_Q 	  Constant Quality (Q) mode 
     */
    cfg.kf_max_dist = global__VPX_KF_MAX_DIST; // a full frame every x frames minimum (can be more often, codec decides automatically)
    cfg.g_threads = 4; // Maximum number of threads to use

    rc = vpx_codec_enc_init(vc->encoder, VIDEO_CODEC_ENCODER_INTERFACE, &cfg, VPX_CODEC_USE_FRAME_THREADING);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to initialize encoder: %s", vpx_codec_err_to_string(rc));
        goto BASE_CLEANUP_1;
    }

    /*
    Codec control function to set encoder internal speed settings.
    Changes in this value influences, among others, the encoder's selection of motion estimation methods.
    Values greater than 0 will increase encoder speed at the expense of quality.

    Note:
      Valid range for VP8: -16..16 
      Valid range for VP9: -8..8
    */
    rc = vpx_codec_control(vc->encoder, VP8E_SET_CPUUSED, global__VP8E_SET_CPUUSED_VALUE);

    if (rc != VPX_CODEC_OK) {
        LOGGER_ERROR(log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
        vpx_codec_destroy(vc->encoder);
        goto BASE_CLEANUP_1;
    }

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

    vc->linfts = current_time_monotonic();
    vc->lcfd = 60;
    vc->vcb.first = cb;
    vc->vcb.second = cb_data;
    vc->friend_number = friend_number;
    vc->av = av;
    vc->log = log;

    return vc;

BASE_CLEANUP_1:
    vpx_codec_destroy(vc->decoder);
BASE_CLEANUP:
    pthread_mutex_destroy(vc->queue_mutex);
    rb_kill((RingBuffer *)vc->vbuf_raw);
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

    while (rb_read((RingBuffer *)vc->vbuf_raw, &p)) {
        free(p);
    }

    rb_kill((RingBuffer *)vc->vbuf_raw);

    pthread_mutex_destroy(vc->queue_mutex);

    LOGGER_DEBUG(vc->log, "Terminated video handler: %p", vc);
    free(vc);
}

void vc_iterate(VCSession *vc)
{
    if (!vc) {
        return;
    }

    struct RTPMessage *p;

    vpx_codec_err_t rc;

    pthread_mutex_lock(vc->queue_mutex);

    if (rb_read((RingBuffer *)vc->vbuf_raw, (void **)&p)) {
        pthread_mutex_unlock(vc->queue_mutex);

        LOGGER_ERROR(vc->log, "vc_iterate: rb_read");

        rc = vpx_codec_decode(vc->decoder, p->data, p->len, NULL, global__MAX_DECODE_TIME_US);
        free(p);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Error decoding video: %s", vpx_codec_err_to_string(rc));
        } else {
            vpx_codec_iter_t iter = NULL;
            vpx_image_t *dest = vpx_codec_get_frame(vc->decoder, &iter);
            LOGGER_ERROR(vc->log, "vpx_codec_get_frame=%p", dest);

            if (dest != NULL)
	    {
                if (vc->vcb.first) {
                    vc->vcb.first(vc->av, vc->friend_number, dest->d_w, dest->d_h,
                                  (const uint8_t *)dest->planes[0], (const uint8_t *)dest->planes[1], (const uint8_t *)dest->planes[2],
                                  dest->stride[0], dest->stride[1], dest->stride[2], vc->vcb.second);
                }
		vpx_img_free(dest);
	    }

            /* Play decoded images */
            for (; dest; dest = vpx_codec_get_frame(vc->decoder, &iter)) {
                if (vc->vcb.first) {
                    vc->vcb.first(vc->av, vc->friend_number, dest->d_w, dest->d_h,
                                  (const uint8_t *)dest->planes[0], (const uint8_t *)dest->planes[1], (const uint8_t *)dest->planes[2],
                                  dest->stride[0], dest->stride[1], dest->stride[2], vc->vcb.second);
                }

                vpx_img_free(dest);
            }
        }

        return;
    }
    else
    {
        LOGGER_ERROR(vc->log, "Error decoding video: rb_read");
    }

    pthread_mutex_unlock(vc->queue_mutex);
}

int vc_queue_message(void *vcp, struct RTPMessage *msg)
{
    /* This function does the reconstruction of video packets.
     * See more info about video splitting in docs
     */
    if (!vcp || !msg) {
        return -1;
    }

    VCSession *vc = (VCSession *)vcp;

    if (msg->header.pt == (rtp_TypeVideo + 2) % 128) {
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
    void *ret = rb_write((RingBuffer *)vc->vbuf_raw, msg);
    LOGGER_WARNING(vc->log, "vc_queue_message:rb_write ret=%p", ret);
    free(ret);

   //{
        /* Calculate time took for peer to send us this frame */
        uint32_t t_lcfd = current_time_monotonic() - vc->linfts;
        vc->lcfd = t_lcfd > 100 ? vc->lcfd : t_lcfd;
        vc->linfts = current_time_monotonic();
    //}

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

    if ((cfg.rc_target_bitrate == bit_rate && cfg.g_w == width && cfg.g_h == height)
		&& (global__VP8E_SET_CPUUSED_VALUE__prev_value == global__VP8E_SET_CPUUSED_VALUE)
		&& (global__VPX_END_USAGE__prev_value == global__VPX_END_USAGE)
		&& (global__VPX_KF_MAX_DIST__prev_value == global__VPX_KF_MAX_DIST)
		&& (global__VPX_G_LAG_IN_FRAMES__prev_value == global__VPX_G_LAG_IN_FRAMES)
		&& (global__MAX_ENCODE_TIME_US__prev_value == global__MAX_ENCODE_TIME_US)
		) {
        return 0; /* Nothing changed */
    }

		global__VP8E_SET_CPUUSED_VALUE__prev_value = global__VP8E_SET_CPUUSED_VALUE;
		global__VPX_END_USAGE__prev_value = global__VPX_END_USAGE;
		global__VPX_KF_MAX_DIST__prev_value = global__VPX_KF_MAX_DIST;
		global__VPX_G_LAG_IN_FRAMES__prev_value = global__VPX_G_LAG_IN_FRAMES;
		global__MAX_ENCODE_TIME_US__prev_value = global__MAX_ENCODE_TIME_US;

        LOGGER_DEBUG(vc->log, "Have to reinitialize vpx encoder on session %p", vc);

        cfg.rc_target_bitrate = bit_rate;
        cfg.g_w = width;
        cfg.g_h = height;

		cfg.rc_end_usage = global__VPX_END_USAGE; // what quality mode?
		/*
		VPX_VBR 	Variable Bit Rate (VBR) mode
		VPX_CBR 	Constant Bit Rate (CBR) mode
		VPX_CQ 	Constrained Quality (CQ) mode -> give codec a hint that we may be on low bandwidth connection
		VPX_Q 	  Constant Quality (Q) mode 
		*/

		cfg.kf_max_dist = global__VPX_KF_MAX_DIST;
		cfg.g_lag_in_frames = global__VPX_G_LAG_IN_FRAMES;


        LOGGER_ERROR(vc->log, "encoder: global__VPX_END_USAGE=%d, global__VP8E_SET_CPUUSED_VALUE=%d", (int)global__VPX_END_USAGE, (int)global__VP8E_SET_CPUUSED_VALUE);

        vpx_codec_ctx_t new_c;

        rc = vpx_codec_enc_init(&new_c, VIDEO_CODEC_ENCODER_INTERFACE, &cfg, VPX_CODEC_USE_FRAME_THREADING);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Failed to initialize encoder: %s", vpx_codec_err_to_string(rc));
            return -1;
        }

        rc = vpx_codec_control(&new_c, VP8E_SET_CPUUSED, global__VP8E_SET_CPUUSED_VALUE);

        if (rc != VPX_CODEC_OK) {
            LOGGER_ERROR(vc->log, "Failed to set encoder control setting: %s", vpx_codec_err_to_string(rc));
            vpx_codec_destroy(&new_c);
            return -1;
        }

        vpx_codec_destroy(vc->encoder);
        memcpy(vc->encoder, &new_c, sizeof(new_c));

    return 0;
}

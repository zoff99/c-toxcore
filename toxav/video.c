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
#include "../toxcore/mono_time.h"


#include "tox_generic.h"

#include "codecs/toxav_codecs.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

/* activate only for debugging!! */
// #define DEBUG_SHOW_H264_DECODING_TIME 1
/* activate only for debugging!! */

/*
 * Zoff: disable logging in ToxAV for now
 */
#include <stdio.h>


#undef LOGGER_DEBUG
#define LOGGER_DEBUG(log, ...) printf("")
#undef LOGGER_ERROR
#define LOGGER_ERROR(log, ...) printf("")
#undef LOGGER_WARNING
#define LOGGER_WARNING(log, ...) printf("")
#undef LOGGER_INFO
#define LOGGER_INFO(log, ...) printf("")

/*
 * Zoff: disable logging in ToxAV for now
 */

/*
 * return -1 on failure, 0 on success
 *
 */

int video_send_custom_lossless_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length);

int video_send_custom_lossless_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossless_packet(tox, friendnumber, data, (size_t)length, &error);

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 0;
    }

    return -1;
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
    vc->rountrip_time_ms = 300; // set xxx ms rountrip network time, before we get an actual value calculated
    vc->has_rountrip_time_ms = 0;
    vc->pinned_to_rountrip_time_ms = 0;
    vc->video_play_delay = 0;
    vc->video_play_delay_real = 0;
    vc->video_frame_buffer_entries = 0;
    vc->parsed_h264_sps_profile_i = 0;
    vc->parsed_h264_sps_level_i = 0;
    vc->last_sent_keyframe_ts = 0;
    vc->video_incoming_frame_orientation = TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_0;

    vc->last_incoming_frame_ts = 0;
    vc->last_parsed_h264_sps_ts = 0;
    vc->timestamp_difference_to_sender__for_video = 0;
    vc->timestamp_difference_adjustment = -450;
    vc->video_received_first_frame = 0;
    vc->tsb_range_ms = 60;
    vc->startup_video_timespan = 8000;
    vc->incoming_video_bitrate_last_changed = 0;
    vc->network_round_trip_time_last_cb_ts = 0;
    vc->network_round_trip_adjustment = 0;
    vc->incoming_video_bitrate_last_cb_ts = 0;
    vc->last_requested_lower_fps_ts = 0;
    vc->encoder_frame_has_record_timestamp = 1;
    vc->video_max_bitrate = VIDEO_BITRATE_MAX_AUTO_VALUE_H264;
    // -----------------------------------
    vc->video_decoder_buffer_ms = 0;
    vc->video_decoder_add_delay_ms = 0;
    // -----------------------------------
    vc->video_decoder_caused_delay_ms = 0;
    vc->client_video_capture_delay_ms = 0;
    vc->remote_client_video_capture_delay_ms = 0;
    vc->global_decode_first_frame_delayed_by = 0;
    vc->global_decode_first_frame_delayed_ms = 0;
    vc->global_decode_first_frame_got = 0;
    // options ---

    vc->incoming_video_frames_gap_ms_index = 0;
    vc->incoming_video_frames_gap_last_ts = 0;
    vc->incoming_video_frames_gap_ms_mean_value = 0;

    vc->video_decoder_caused_delay_ms_array_index = 0;
    vc->video_decoder_caused_delay_ms_mean_value = 0;

    vc->video_buf_ms_array_index = 0;
    vc->video_buf_ms_mean_value = 0;

    vc->video_buf_ms_array_index_long = 0;
    vc->video_buf_ms_mean_value_long = 0;

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

// static int global_last_viterate_ts = 0;
// static int global___ts1 = 0;

/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */
/* --- VIDEO DECODING happens here --- */
uint8_t vc_iterate(VCSession *vc, Tox *tox, uint8_t skip_video_flag, uint64_t *a_r_timestamp,
                   uint64_t *a_l_timestamp,
                   uint64_t *v_r_timestamp, uint64_t *v_l_timestamp, BWController *bwc,
                   int64_t *timestamp_difference_adjustment_for_audio,
                   int64_t *timestamp_difference_to_sender_,
                   int32_t *video_has_rountrip_time_ms)
{

    if (!vc) {
        return 0;
    }

    uint8_t ret_value = 0;
    struct RTPMessage *p;
    bool have_requested_index_frame = false;

    vpx_codec_err_t rc = 0;

    pthread_mutex_lock(vc->queue_mutex);

    uint64_t frame_flags;
    uint8_t data_type;
    uint8_t h264_encoded_video_frame = 0;

    uint32_t full_data_len;

    uint32_t timestamp_out_ = 0;
    uint32_t timestamp_min = 0;
    uint32_t timestamp_max = 0;

    *timestamp_difference_to_sender_ = vc->timestamp_difference_to_sender__for_video;

    tsb_get_range_in_buffer(tox, (TSBuffer *)vc->vbuf_raw, &timestamp_min, &timestamp_max);


    /**
     * this is the magic value that drifts with network delay changes:
     * vc->timestamp_difference_adjustment
     */
    int want_remote_video_ts = ((int)current_time_monotonic(vc->av->toxav_mono_time) +
                                (int)vc->timestamp_difference_to_sender__for_video +
                                (int)vc->timestamp_difference_adjustment -
                                (int)vc->video_decoder_buffer_ms);
    LOGGER_API_DEBUG(tox, "want_remote_video_ts:v:002=%d, %d %d %d %d",
            (int)want_remote_video_ts,
            (int)current_time_monotonic(vc->av->toxav_mono_time),
            (int)vc->timestamp_difference_to_sender__for_video,
            (int)vc->timestamp_difference_adjustment,
            (int)vc->video_decoder_buffer_ms
            );
    /**
     *
     */


    LOGGER_API_DEBUG(tox, "VC_TS_CALC:01:%d %d %d %d",
                     (int)want_remote_video_ts,
                     (int)(current_time_monotonic(vc->av->toxav_mono_time)),
                     (int)vc->timestamp_difference_to_sender__for_video,
                     (int)vc->timestamp_difference_adjustment
                    );

    uint32_t timestamp_want_get = (int)want_remote_video_ts;

    // HINT: compensate for older clients ----------------
    if (vc->encoder_frame_has_record_timestamp == 0) {
        LOGGER_API_DEBUG(tox, "old client:002");
        vc->tsb_range_ms = UINT32_MAX;
        timestamp_want_get = UINT32_MAX;
        vc->startup_video_timespan = 0;
    }

    // HINT: compensate for older clients ----------------

    LOGGER_API_DEBUG(tox, "FC:%d min=%d max=%d want=%d diff=%d adj=%d roundtrip=%d",
                 (int)tsb_size((TSBuffer *)vc->vbuf_raw),
                 timestamp_min,
                 timestamp_max,
                 (int)timestamp_want_get,
                 (int)timestamp_want_get - (int)timestamp_max,
                 (int)vc->timestamp_difference_adjustment,
                 (int)vc->rountrip_time_ms);


    int32_t video_frame_diff = (int)timestamp_want_get - (int)timestamp_max;
    int has_adjusted = 0;

    // ------- calc mean value -------
    if ((video_frame_diff > -800) && (video_frame_diff < 3000))
    {
        vc->video_buf_ms_array[vc->video_buf_ms_array_index] = video_frame_diff + 1000;
        vc->video_buf_ms_array_index = (vc->video_buf_ms_array_index + 1) %
                VIDEO_BUF_MS_ENTRIES;

        int32_t mean_value = 0;

        for (int k = 0; k < VIDEO_BUF_MS_ENTRIES; k++) {
            mean_value = mean_value + vc->video_buf_ms_array[k];
        }

        if (mean_value == 0) {
        } else {
            vc->video_buf_ms_mean_value = (mean_value / VIDEO_BUF_MS_ENTRIES) - 1000;
        }
    }
    // ------- calc mean value -------


    // ------- calc mean value -------
    if ((video_frame_diff > -3000) && (video_frame_diff < 20000))
    {
        vc->video_buf_ms_array_long[vc->video_buf_ms_array_index_long] = video_frame_diff + 4000;
        vc->video_buf_ms_array_index_long = (vc->video_buf_ms_array_index_long + 1) %
                VIDEO_BUF_MS_ENTRIES_LONG;

        int32_t mean_value = 0;

        for (int k = 0; k < VIDEO_BUF_MS_ENTRIES_LONG; k++) {
            mean_value = mean_value + vc->video_buf_ms_array_long[k];
        }

        if (mean_value == 0) {
        } else {
            vc->video_buf_ms_mean_value_long = (mean_value / VIDEO_BUF_MS_ENTRIES_LONG) - 4000;
        }
    }
    // ------- calc mean value -------


    LOGGER_API_DEBUG(tox, "rtt:drift:vfd:a:rtt=%d adj=%d cur=%d m=%d ml=%d",
                    (int32_t)vc->rountrip_time_ms,
                    (int)(vc->timestamp_difference_adjustment),
                    video_frame_diff,
                    vc->video_buf_ms_mean_value,
                    vc->video_buf_ms_mean_value_long);

    if ((vc->pinned_to_rountrip_time_ms == 0) && (vc->has_rountrip_time_ms == 1))
    {
        // ------------ pin play out timestamps to (RTT/2 + "GUESS_REMOTE_ENCODER_DELAY_MS" ms)
        vc->timestamp_difference_adjustment = -(int)((vc->rountrip_time_ms/2) + GUESS_REMOTE_ENCODER_DELAY_MS);
        vc->network_round_trip_adjustment = vc->rountrip_time_ms;
        LOGGER_API_DEBUG(tox, "adj:drift:5a:jmp:%d video_frame_diff=%d rtt_adj=%d", (int)(vc->timestamp_difference_adjustment), video_frame_diff, vc->network_round_trip_adjustment);
        vc->pinned_to_rountrip_time_ms = 1;
    }

    LOGGER_API_DEBUG(tox, "adj:xxxx:9:%d video_frame_diff=%d rtt_adj=%d", (int)(vc->timestamp_difference_adjustment), video_frame_diff, vc->network_round_trip_adjustment);

    if (vc->has_rountrip_time_ms == 1)
    {
        // im RTT changed by 2ms in either direction then drift network_round_trip_adjustment in that way by 1ms
        if (vc->network_round_trip_adjustment > ((int32_t)vc->rountrip_time_ms + 1))
        {
            vc->network_round_trip_adjustment--;
            vc->network_round_trip_adjustment--;
            vc->timestamp_difference_adjustment++;
            LOGGER_API_DEBUG(tox, "adj:drift:aa++");
        }
        else if (vc->network_round_trip_adjustment < ((int32_t)vc->rountrip_time_ms - 1))
        {
            vc->network_round_trip_adjustment++;
            vc->network_round_trip_adjustment++;
            vc->timestamp_difference_adjustment--;
            LOGGER_API_DEBUG(tox, "adj:drift:aa-----");
        }
    }


    uint16_t removed_entries;
    uint16_t is_skipping = 0;

    uint32_t tsb_range_ms_used = vc->tsb_range_ms + vc->startup_video_timespan;
    uint32_t timestamp_want_get_used = timestamp_want_get;
    LOGGER_API_DEBUG(tox,"timestamp_want_get_used:001=%d", (int)timestamp_want_get_used);

    int use_range_all = 0;
    if ((vc->video_received_first_frame == 0) || (vc->has_rountrip_time_ms == 0) || ((video_frame_diff > 1000) && (video_frame_diff < 10000))) {
        tsb_range_ms_used = UINT32_MAX;
        timestamp_want_get_used = UINT32_MAX;
        use_range_all = 1;
        LOGGER_API_DEBUG(tox,"first_frame:001:timestamp_want_get_used:002=%d", (int)timestamp_want_get_used);
    }

    if (use_range_all == 1)
    {
        // this will force audio stream to play anything that comes in without timestamps
        *video_has_rountrip_time_ms = 0;
    }
    else
    {
        *video_has_rountrip_time_ms = vc->has_rountrip_time_ms;
    }


    if ((video_frame_diff > 1000) && (video_frame_diff < 10000))
    {
        LOGGER_API_DEBUG(tox, "video frames are delayed[a] for more than 1000ms (%d ms), turn down bandwidth fast", (int)video_frame_diff);
        bwc_add_lost_v3(bwc, 199999, true);
    }
#if 1
    else if ((video_frame_diff > 800) && (video_frame_diff < 10000))
    {
        LOGGER_API_DEBUG(tox, "video frames are delayed[b] for more than 800ms (%d ms), turn down bandwidth", (int)video_frame_diff);
        bwc_add_lost_v3(bwc, 60, true);
    }
#endif
#if 1
    else if ((vc->has_rountrip_time_ms == 1) && (video_frame_diff > 1) && (video_frame_diff > ((vc->rountrip_time_ms) + 100)) && (video_frame_diff < 10000))
    {
        //if ((uint32_t)tsb_size((TSBuffer *)vc->vbuf_raw) > 0)
        {
            if ((vc->rountrip_time_ms > 300) && (vc->rountrip_time_ms < 1000))
            {
                LOGGER_API_DEBUG(tox, "video frames are delayed[c] (%d ms, RTT=%d ms), turn down bandwidth", (int)video_frame_diff, (int)vc->rountrip_time_ms);
                bwc_add_lost_v3(bwc, 3, true);
            }
        }
    }
#endif
#if 0
    else if ((vc->has_rountrip_time_ms == 1) && (vc->video_buf_ms_mean_value < 0) && (vc->video_buf_ms_mean_value > -200))
    {
        if ((vc->rountrip_time_ms > 300) && (vc->rountrip_time_ms < 800))
        {
            if ((vc->video_decoder_buffer_ms > 0) && (vc->video_decoder_buffer_ms < 298))
            {
                // TODO: the problem is this buffer increase will never be turned back.
                //       let's not do this for now.
                vc->video_decoder_buffer_ms++;
                LOGGER_API_DEBUG(tox, "video frames are delayed[d] (%d ms, RTT=%d ms dbuf=%d ms), add more buffering", (int)vc->video_buf_ms_mean_value, (int)vc->rountrip_time_ms, (int)vc->video_decoder_buffer_ms);
            }
        }
    }
#endif

    if ((video_frame_diff > ((int)vc->video_buf_ms_mean_value_long + 300)) && (video_frame_diff < 100000))
    {
        bwc_add_lost_v3(bwc, 70, true);
        LOGGER_API_DEBUG(tox, "video frames are delayed[e], (vdf=%d RTT=%d ml=%d) turn down bandwidth",
            (int)video_frame_diff,
            (int)vc->rountrip_time_ms,
            (int)vc->video_buf_ms_mean_value_long);
    }

    LOGGER_API_DEBUG(tox, "tsb_read got: want=%d %d %d %d %d",
                     (int)timestamp_want_get_used,
                     (int)(vc->tsb_range_ms),
                     (int)(vc->startup_video_timespan),
                     (int)(tsb_range_ms_used),
                     (int)tsb_size((TSBuffer *)vc->vbuf_raw)
                    );


    // HINT: give me video frames that happend "now" minus some diff
    //       get a videoframe for timestamp [timestamp_want_get_used]
    if (tsb_read((TSBuffer *)vc->vbuf_raw, vc->log, (void **)&p, &frame_flags,
                 &timestamp_out_,
                 timestamp_want_get_used,
                 tsb_range_ms_used,
                 &removed_entries,
                 &is_skipping)) {

        if (vc->video_received_first_frame == 0) {
            vc->video_received_first_frame = 1;
        }

        const struct RTPHeader *header_v3_0 = (void *) & (p->header);

        LOGGER_API_DEBUG(tox, "XLS01:%d,%d, diff_got=%d",
                         (int)(timestamp_want_get - current_time_monotonic(vc->av->toxav_mono_time)),
                         (int)(timestamp_out_ - current_time_monotonic(vc->av->toxav_mono_time)),
                         (int)(timestamp_want_get - timestamp_out_)
                        );

        vc->video_play_delay = ((current_time_monotonic(vc->av->toxav_mono_time) +
                                 vc->timestamp_difference_to_sender__for_video) - timestamp_out_);
        vc->video_play_delay_real = vc->video_play_delay + vc->video_decoder_caused_delay_ms;

        vc->video_frame_buffer_entries = (uint32_t)tsb_size((TSBuffer *)vc->vbuf_raw);

        if (removed_entries > 0) {

            LOGGER_API_DEBUG(tox,
                             "seq:%d FC:%d min=%d max=%d want=%d hgot=%d got=%d diff=%d rm=%d pdelay=%d pdelayr=%d adj=%d dts=%d rtt=%d decoder_delay=%d",
                             (int)header_v3_0->sequnum,
                             (int)tsb_size((TSBuffer *)vc->vbuf_raw),
                             timestamp_min,
                             timestamp_max,
                             (int)timestamp_want_get,
                             (int)header_v3_0->frame_record_timestamp,
                             (int)timestamp_out_,
                             ((int)timestamp_want_get - (int)timestamp_out_),
                             (int)removed_entries,
                             (int)vc->video_play_delay,
                             (int)vc->video_play_delay_real,
                             (int)vc->timestamp_difference_adjustment,
                             (int)vc->timestamp_difference_to_sender__for_video,
                             (int)vc->rountrip_time_ms,
                             (int)vc->video_decoder_caused_delay_ms);
        }

        uint16_t buf_size = tsb_size((TSBuffer *)vc->vbuf_raw);
        int32_t diff_want_to_got = (int)timestamp_want_get - (int)timestamp_out_;


        LOGGER_API_DEBUG(tox, "values:diff_to_sender=%d adj=%d tsb_range=%d bufsize=%d",
                         (int)vc->timestamp_difference_to_sender__for_video, (int)vc->timestamp_difference_adjustment,
                         (int)vc->tsb_range_ms,
                         (int)buf_size);


        if (vc->startup_video_timespan > 0) {
            vc->startup_video_timespan = 0;
        }



        // TODO: calculate the delay for the audio stream, and pass it back
        // bad hack -> make better!
        // ----------------------------------------
        // ----------------------------------------
        // ----------------------------------------
        int32_t delay_audio_stream_relative_to_video_stream = (vc->video_decoder_buffer_ms + vc->video_decoder_add_delay_ms);
        //if ((vc->video_decoder_add_delay_ms < 0) && ((-(vc->video_decoder_add_delay_ms)) > vc->video_decoder_buffer_ms))
        //{
        //    delay_audio_stream_relative_to_video_stream = 0;
        //}

        uint32_t video_decoder_caused_delay_ms_mean_value_used = vc->video_decoder_caused_delay_ms_mean_value;
        if (video_decoder_caused_delay_ms_mean_value_used > 300)
        {
            video_decoder_caused_delay_ms_mean_value_used = 300;
        }

        *timestamp_difference_adjustment_for_audio = vc->timestamp_difference_adjustment -
                delay_audio_stream_relative_to_video_stream -
                vc->video_decoder_caused_delay_ms_mean_value;
        LOGGER_API_DEBUG(tox, "want_remote_video_ts:v:003=%d", (int)timestamp_difference_adjustment_for_audio);
        LOGGER_API_DEBUG(tox, "VV:01:%d", (int)vc->video_decoder_buffer_ms);
        LOGGER_API_DEBUG(tox, "VV:02:%d %d", (int)*timestamp_difference_adjustment_for_audio, (int)vc->timestamp_difference_adjustment);
        LOGGER_API_DEBUG(tox, "VV:03:%d %d", (int)vc->video_decoder_add_delay_ms, (int)vc->video_decoder_caused_delay_ms_mean_value);
        // ----------------------------------------
        // ----------------------------------------
        // ----------------------------------------


        LOGGER_API_DEBUG(tox, "--VSEQ:%d", (int)header_v3_0->sequnum);

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
            LOGGER_API_ERROR(tox, "skipping incoming video frame (0) with sn=%d lastseen=%d old_frames_count=%d",
                           (int)header_v3_0->sequnum,
                           (int)vc->last_seen_fragment_seqnum,
                           (int)vc->count_old_video_frames_seen);

            vc->count_old_video_frames_seen++;

            if ((int32_t)(header_v3_0->sequnum + 1) != (int32_t)vc->last_seen_fragment_seqnum) {
                // TODO: check why we often get exactly the previous video frame here?!?!
                LOGGER_API_ERROR(tox, "got previous seq number");
            }

            if (vc->count_old_video_frames_seen > 6) {
                // if we see more than 6 old video frames in a row, then either there was
                // a seqnum rollover or something else. just play those frames then
                vc->last_seen_fragment_seqnum = (int32_t)header_v3_0->sequnum;
                vc->count_old_video_frames_seen = 0;
                LOGGER_API_ERROR(tox, "count_old_video_frames_seen > 6");
            }

            free(p);
            pthread_mutex_unlock(vc->queue_mutex);
            return 0;
        }

        if ((int32_t)header_v3_0->sequnum != (int32_t)(vc->last_seen_fragment_seqnum + 1)) {
            int32_t missing_frames_count = (int32_t)header_v3_0->sequnum -
                                           (int32_t)(vc->last_seen_fragment_seqnum + 1);

            LOGGER_API_ERROR(tox, "missing some video frames: missing count=%d", (int)missing_frames_count);

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
                LOGGER_API_ERROR(tox, "missing? sn=%d lastseen=%d",
                               (int)header_v3_0->sequnum,
                               (int)vc->last_seen_fragment_seqnum);


                LOGGER_API_ERROR(tox, "missing %d video frames (m1)", (int)missing_frames_count);
#endif

                if (vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264) {
                    rc = vpx_codec_decode(vc->decoder, NULL, 0, NULL, VPX_DL_REALTIME);
                }

                // HINT: give feedback that we lost some bytes (based on the size of this frame)
                bwc_add_lost_v3(bwc, (uint32_t)(header_v3_0->data_length_full * missing_frames_count), true);
#ifndef RPIZEROW
                LOGGER_API_ERROR(tox, "BWC:lost:002:missing count=%d", (int)missing_frames_count);
#endif
            }
        }


        // TODO: check for seqnum rollover!!
        vc->count_old_video_frames_seen = 0;
        vc->last_seen_fragment_seqnum = header_v3_0->sequnum;

        //* PREVIOUS UNLOCK *//
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

            bwc_add_lost_v3(bwc, (full_data_len - header_v3->received_length_full), false);

            float percent_lost = 0.0f;

            if (header_v3->received_length_full > 0) {
                percent_lost = (float)full_data_len / (float)header_v3->received_length_full;
            }

            LOGGER_API_ERROR(tox, "BWC:lost:004:lost bytes=%d recevied=%d full=%d per=%.3f",
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

        // HINT: sometimes the singaling of H264 capability does not work
        //       as workaround send it again on the first 30 frames

        if (DISABLE_H264_DECODER_FEATURE != 1) {
            if ((vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264)
                    && ((long)header_v3->sequnum < 30)) {

                // HINT: tell friend that we have H264 decoder capabilities (3) -------
                uint32_t pkg_buf_len = 2;
                uint8_t pkg_buf[pkg_buf_len];
                pkg_buf[0] = PACKET_TOXAV_COMM_CHANNEL;
                pkg_buf[1] = PACKET_TOXAV_COMM_CHANNEL_HAVE_H264_VIDEO;

                int result = video_send_custom_lossless_packet(tox, vc->friend_number, pkg_buf, pkg_buf_len);
                LOGGER_ERROR(vc->log, "PACKET_TOXAV_COMM_CHANNEL_HAVE_H264_VIDEO=%d\n", (int)result);
                // HINT: tell friend that we have H264 decoder capabilities -------

            }
        }

        if (vc->video_decoder_codec_used != TOXAV_ENCODER_CODEC_USED_H264) {
            // LOGGER_ERROR(vc->log, "DEC:VP8------------");
            decode_frame_vpx(vc, tox, skip_video_flag, a_r_timestamp,
                             a_l_timestamp,
                             v_r_timestamp, v_l_timestamp,
                             header_v3, p,
                             rc, full_data_len,
                             &ret_value);
        } else {
            // LOGGER_ERROR(vc->log, "DEC:H264------------");

#ifdef DEBUG_SHOW_H264_DECODING_TIME
            uint32_t start_time_ms = current_time_monotonic(vc->av->toxav_mono_time);
#endif
            decode_frame_h264(vc, tox, skip_video_flag, a_r_timestamp,
                              a_l_timestamp,
                              v_r_timestamp, v_l_timestamp,
                              header_v3, p,
                              rc, full_data_len,
                              &ret_value);

#ifdef DEBUG_SHOW_H264_DECODING_TIME
            uint32_t end_time_ms = current_time_monotonic(vc->av->toxav_mono_time);

            if ((int)(end_time_ms - start_time_ms) > 4) {
                LOGGER_API_WARNING(tox, "decode_frame_h264: %d ms", (int)(end_time_ms - start_time_ms));
            }

#endif
        }

//        LOGGER_DEBUG(vc->log, "vciter__01:%d",
//                            (int)current_time_monotonic(m->mono_time) - global___ts1);

        //* NEW UNLOCK *//
        // pthread_mutex_unlock(vc->queue_mutex);

        return ret_value;
    } else {
        // no frame data available
        // LOGGER_WARNING(vc->log, "Error decoding video: rb_read");
        if (removed_entries > 0) {
            LOGGER_API_ERROR(tox, "no frame read, but removed entries=%d", (int)removed_entries);
        }
    }

    pthread_mutex_unlock(vc->queue_mutex);

    // LOGGER_DEBUG(vc->log, "vciter__02:%d",
    //                    (int)current_time_monotonic(m->mono_time) - global___ts1);

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

    pthread_mutex_lock(vc->queue_mutex);


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

    LOGGER_API_DEBUG(vc->av->tox, "TT:queue:V:fragnum=%ld", (long)header_v3->fragment_num);

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


            LOGGER_API_DEBUG(vc->av->tox, "vc_queue_msg:tsb_write : %d", (uint32_t)header->frame_record_timestamp);

            struct RTPMessage *msg_old = tsb_write((TSBuffer *)vc->vbuf_raw, msg,
                                                   (uint64_t)header->flags,
                                                   (uint32_t)header->frame_record_timestamp);

            if (msg_old) {
                LOGGER_API_DEBUG(vc->av->tox, "FPATH:%d kicked out", (int)msg_old->header.sequnum);
                free(msg_old);
            }
        } else {
            // discard incoming frame, we want to see our outgoing frames instead
            if (msg) {
                free(msg);
            }
        }
    } else {
        free(tsb_write((TSBuffer *)vc->vbuf_raw, msg, 0, current_time_monotonic(mono_time)));
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
    int ret = 0;
    
    if (vc->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8) {
        ret = vc_reconfigure_encoder_vpx(log, vc, bit_rate, width, height, kf_max_dist);
    } else {
#ifdef RASPBERRY_PI_OMX
        ret = vc_reconfigure_encoder_h264_omx_raspi(log, vc, bit_rate, width, height, kf_max_dist);
#else
        ret = vc_reconfigure_encoder_h264(log, vc, bit_rate, width, height, kf_max_dist);
#endif
    }

    vc->video_encoder_coded_used_prev = vc->video_encoder_coded_used;
    return ret;
}


/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "audio.h"

#include "ring_buffer.h"
#include "ts_buffer.h"
#include "rtp.h"

#include "../toxcore/logger.h"
#include "../toxcore/mono_time.h"

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


static struct TSBuffer *jbuf_new(int size);
static void jbuf_free(struct TSBuffer *q);
static int jbuf_write(Logger *log, ACSession *ac, struct TSBuffer *q, struct RTPMessage *m);
static OpusEncoder *create_audio_encoder(const Logger *log, int32_t bit_rate, int32_t sampling_rate,
        int32_t channel_count);
static bool reconfigure_audio_encoder(const Logger *log, OpusEncoder **e, int32_t new_br, int32_t new_sr,
                                      uint8_t new_ch, int32_t *old_br, int32_t *old_sr, int32_t *old_ch);
static bool reconfigure_audio_decoder(ACSession *ac, int32_t sampling_rate, int8_t channels);



ACSession *ac_new(Mono_Time *mono_time, const Logger *log, ToxAV *av, Tox *tox, uint32_t friend_number,
                  toxav_audio_receive_frame_cb *cb, void *cb_data)
{
    ACSession *ac = (ACSession *)calloc(sizeof(ACSession), 1);

    if (!ac) {
        LOGGER_WARNING(log, "Allocation failed! Application might misbehave!");
        return nullptr;
    }

    if (create_recursive_mutex(ac->queue_mutex) != 0) {
        LOGGER_WARNING(log, "Failed to create recursive mutex!");
        free(ac);
        return nullptr;
    }

    int status;
    ac->decoder = opus_decoder_create(AUDIO_DECODER__START_SAMPLING_RATE, AUDIO_DECODER__START_CHANNEL_COUNT, &status);

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while starting audio decoder: %s", opus_strerror(status));
        goto BASE_CLEANUP;
    }

    if (!(ac->j_buf = jbuf_new(AUDIO_JITTERBUFFER_COUNT))) {
        LOGGER_WARNING(log, "Jitter buffer creaton failed!");
        opus_decoder_destroy(ac->decoder);
        goto BASE_CLEANUP;
    }

    ac->mono_time = mono_time;

    /* Initialize encoders with default values */
    ac->encoder = create_audio_encoder(log, AUDIO_START_BITRATE_RATE, AUDIO_START_SAMPLING_RATE, AUDIO_START_CHANNEL_COUNT);

    if (ac->encoder == nullptr) {
        goto DECODER_CLEANUP;
    } else {
        LOGGER_INFO(log, "audio encoder successfully created");
    }

    ac->le_bit_rate = AUDIO_START_BITRATE_RATE;
    ac->le_sample_rate = AUDIO_START_SAMPLING_RATE;
    ac->le_channel_count = AUDIO_START_CHANNEL_COUNT;

    ac->ld_channel_count = AUDIO_DECODER__START_SAMPLING_RATE;
    ac->ld_sample_rate = AUDIO_DECODER__START_CHANNEL_COUNT;
    ac->ldrts = 0; /* Make it possible to reconfigure straight away */

    ac->lp_seqnum_new = -1;

    ac->last_incoming_frame_ts = 0;
    ac->timestamp_difference_to_sender = 0; // no difference to sender as start value

    /* These need to be set in order to properly
     * do error correction with opus */
    ac->lp_frame_duration = AUDIO_MAX_FRAME_DURATION_MS;
    ac->lp_sampling_rate = AUDIO_DECODER__START_SAMPLING_RATE;
    ac->lp_channel_count = AUDIO_DECODER__START_CHANNEL_COUNT;

    ac->encoder_frame_has_record_timestamp = 1;
    ac->audio_received_first_frame = 0;

    ac->av = av;
    ac->tox = tox;
    ac->friend_number = friend_number;
    ac->acb = cb;
    ac->acb_user_data = cb_data;

    return ac;

DECODER_CLEANUP:
    opus_decoder_destroy(ac->decoder);

    jbuf_free((struct TSBuffer *)ac->j_buf);

BASE_CLEANUP:
    pthread_mutex_destroy(ac->queue_mutex);
    free(ac);
    return nullptr;
}

void ac_kill(ACSession *ac)
{
    if (!ac) {
        return;
    }

    opus_encoder_destroy(ac->encoder);
    opus_decoder_destroy(ac->decoder);

    jbuf_free((struct TSBuffer *)ac->j_buf);

    pthread_mutex_destroy(ac->queue_mutex);

    LOGGER_DEBUG(ac->log, "Terminated audio handler: %p", (void *)ac);
    free(ac);
}

// static int global_last_aiterate_ts = 0;

static inline struct RTPMessage *jbuf_read(Logger *log, struct TSBuffer *q, int32_t *success,
        int64_t timestamp_difference_adjustment_for_audio2,
        int64_t timestamp_difference_to_sender_,
        uint8_t encoder_frame_has_record_timestamp,
        ACSession *ac,
        int video_send_cap,
        int32_t video_has_rountrip_time_ms)
{
#define AUDIO_CURRENT_TS_SPAN_MS 65

    void *ret = NULL;
    uint64_t lost_frame = 0;
    uint32_t timestamp_out_ = 0;


    /**
     * this is the magic value that gives the wanted timestamps:
     */
    int64_t want_remote_video_ts = (current_time_monotonic(ac->mono_time) +
                                    timestamp_difference_to_sender_ +
                                    timestamp_difference_adjustment_for_audio2);
    LOGGER_API_ERROR(ac->tox, "want_remote_video_ts:a:002=%d, %d %d %d",
            (int)want_remote_video_ts,
            (int)current_time_monotonic(ac->mono_time),
            (int)timestamp_difference_to_sender_,
            (int)timestamp_difference_adjustment_for_audio2
            );
    /**
     *
     */

    *success = 0;
    uint16_t removed_entries;

    uint32_t tsb_range_ms = AUDIO_CURRENT_TS_SPAN_MS;

    // HINT: compensate for older clients ----------------
    //       or when only receiving audio (no incoming video)
    if ((encoder_frame_has_record_timestamp == 0) || (video_send_cap == 0)) {
        LOGGER_DEBUG(log, "old client:003");
        tsb_range_ms = UINT32_MAX;
        want_remote_video_ts = UINT32_MAX;
    }

    // HINT: compensate for older clients ----------------


#if 1

    uint32_t timestamp_min = 0;
    uint32_t timestamp_max = 0;
    tsb_get_range_in_buffer(ac->tox, q, &timestamp_min, &timestamp_max);

    if ((int)tsb_size(q) > 0) {
        LOGGER_API_ERROR(ac->tox, "FC:%d min=%d max=%d want=%d diff=%d adj=%d",
                       (int)tsb_size(q),
                       timestamp_min,
                       timestamp_max,
                       (int)want_remote_video_ts,
                       (int)want_remote_video_ts - (int)timestamp_max,
                       (int)timestamp_difference_adjustment_for_audio2);
    }

#endif



    uint32_t tsb_range_ms_used = tsb_range_ms;
    uint32_t timestamp_want_get_used = want_remote_video_ts;

    if ((ac->audio_received_first_frame) == 0 || (video_has_rountrip_time_ms == 0)) {
        LOGGER_API_ERROR(ac->tox, "AA:%d %d", (int)ac->audio_received_first_frame, (int)video_has_rountrip_time_ms);
        tsb_range_ms_used = UINT32_MAX;
        timestamp_want_get_used = UINT32_MAX;
    }


    uint16_t is_skipping;
    bool res = tsb_read(q, log, &ret, &lost_frame,
                        &timestamp_out_,
                        timestamp_want_get_used,
                        tsb_range_ms_used,
                        &removed_entries,
                        &is_skipping);

#if 0

    if (res == true) {
        struct RTPMessage *m_debug = (struct RTPMessage *)ret;
        LOGGER_WARNING(log, "tsb_read got: now=%d iter=%d seq:%d FC:%d is_skipping=%d",
                       (int)current_time_monotonic(ac->mono_time),
                       (int)current_time_monotonic(ac->mono_time) - global_last_aiterate_ts,
                       (int)m_debug->header.sequnum,
                       (int)tsb_size(q),
                       (int)is_skipping);

        global_last_aiterate_ts = (int)current_time_monotonic(ac->mono_time);
    }

#endif

    if (removed_entries > 0) {
        LOGGER_DEBUG(log, "removed entries=%d", (int)removed_entries);
    }

    if (res == true) {
        *success = 1;


        if (ac->audio_received_first_frame == 0) {
            ac->audio_received_first_frame = 1;
        }


        LOGGER_DEBUG(log, "audio_read:done:%ld,%ld", want_remote_video_ts, (int64_t)timestamp_out_);


        struct RTPMessage *m = (struct RTPMessage *)ret;

        LOGGER_DEBUG(log, "AudioFramesIN: header sequnum=%d", (int)m->header.sequnum);

        if (ac->lp_seqnum_new == -1) {
            ac->lp_seqnum_new = m->header.sequnum;
        } else {
            if (
                (
                    ((m->header.sequnum > 5) && (ac->lp_seqnum_new < (UINT16_MAX - 5)))
                ) &&
                (m->header.sequnum <= ac->lp_seqnum_new)
            ) {
                LOGGER_DEBUG(ac->log, "AudioFramesIN: drop pkt num: %d", (int)m->header.sequnum);

                free(ret);
                ret = NULL;
            } else {

                LOGGER_DEBUG(log, "AudioFramesIN: using sequnum=%d", (int)m->header.sequnum);

                if (
                    ((m->header.sequnum > 8) && (ac->lp_seqnum_new < (UINT16_MAX - 7)))
                ) {
                    LOGGER_DEBUG(log, "AudioFramesIN:2: %d %d", (int)ac->lp_seqnum_new, (int)m->header.sequnum);
                    int64_t diff = (m->header.sequnum - ac->lp_seqnum_new);

                    if (diff > 1) {
                        LOGGER_DEBUG(log, "AudioFramesIN: missing %d audio frames sequnum missing=%d",
                                     (int)(diff - 1),
                                     (ac->lp_seqnum_new + 1));
                        lost_frame = 1;
                    }
                }

                ac->lp_seqnum_new = m->header.sequnum;
            }
        }
    }

    LOGGER_DEBUG(log, "jbuf_read:lost_frame=%d", (int)lost_frame);

    if (lost_frame == 1) {
        *success = AUDIO_LOST_FRAME_INDICATOR;
    }

    return (struct RTPMessage *)ret;
}

static inline bool jbuf_is_empty(struct TSBuffer *q)
{
    bool res = tsb_empty(q);
    return res;
}

uint8_t ac_iterate(ACSession *ac, uint64_t *a_r_timestamp, uint64_t *a_l_timestamp, uint64_t *v_r_timestamp,
                   uint64_t *v_l_timestamp,
                   int64_t *timestamp_difference_adjustment_for_audio,
                   int64_t *timestamp_difference_to_sender_,
                   int video_send_cap,
                   int32_t *video_has_rountrip_time_ms)
{
    if (!ac) {
        return 0;
    }

    uint8_t ret_value = 1;

    pthread_mutex_lock(ac->queue_mutex);

    struct TSBuffer *jbuffer = (struct TSBuffer *)ac->j_buf;

    if (jbuf_is_empty(jbuffer)) {
        pthread_mutex_unlock(ac->queue_mutex);
        return 0;
    }

    /* Enough space for the maximum frame size (120 ms 48 KHz stereo audio) */
    //int16_t temp_audio_buffer[AUDIO_MAX_BUFFER_SIZE_PCM16_FOR_FRAME_PER_CHANNEL *
    //                                                                            AUDIO_MAX_CHANNEL_COUNT];

    struct RTPMessage *msg = NULL;
    int rc = 0;


    while ((msg = jbuf_read(nullptr, jbuffer, &rc,
                            *timestamp_difference_adjustment_for_audio,
                            *timestamp_difference_to_sender_,
                            ac->encoder_frame_has_record_timestamp, ac,
                            video_send_cap,
                            *video_has_rountrip_time_ms))
            || rc == AUDIO_LOST_FRAME_INDICATOR) {
        pthread_mutex_unlock(ac->queue_mutex);


        LOGGER_API_DEBUG(ac->tox, "TOXAV:A2V_DELAY:(pos==audio-before-video)%d", (int)(*a_r_timestamp - *v_r_timestamp));


        if (rc == AUDIO_LOST_FRAME_INDICATOR) {
            LOGGER_DEBUG(ac->log, "OPUS correction for lost frame (3)");

            if (ac->lp_sampling_rate > 0) {
                int fs = (ac->lp_sampling_rate * ac->lp_frame_duration) / 1000;
                rc = opus_decode(ac->decoder, NULL, 0, ac->temp_audio_buffer, fs, 1);
            }

            free(msg);
            msg = NULL;
        } else {

            int use_fec = 0;
            /* TODO: check if we have the full data of this frame */

            /* Get values from packet and decode. */
            /* NOTE: This didn't work very well */

            /* Pick up sampling rate from packet */
            if (msg) {
                memcpy(&ac->lp_sampling_rate, msg->data, 4);
                ac->lp_sampling_rate = net_ntohl(ac->lp_sampling_rate);
                ac->lp_channel_count = opus_packet_get_nb_channels(msg->data + 4);
                /* TODO: msg->data + 4
                 * this should be defined, not hardcoded
                 */
            }


            /** NOTE: even though OPUS supports decoding mono frames with stereo decoder and vice versa,
              * it didn't work quite well.
              */
            if (!reconfigure_audio_decoder(ac, ac->lp_sampling_rate, ac->lp_channel_count)) {
                LOGGER_WARNING(ac->log, "Failed to reconfigure decoder!");
                free(msg);
                msg = NULL;
                continue;
            }

            /*
            frame_size = opus_decode(dec, packet, len, decoded, max_size, decode_fec);
              where
            packet is the byte array containing the compressed data
            len is the exact number of bytes contained in the packet
            decoded is the decoded audio data in opus_int16 (or float for opus_decode_float())
            max_size is the max duration of the frame in samples (per channel) that can fit
            into the decoded_frame array
            decode_fec: Flag (0 or 1) to request that any in-band forward error correction data be
            decoded. If no such data is available, the frame is decoded as if it were lost.
             */
            /* TODO: msg->data + 4, msg->len - 4
             * this should be defined, not hardcoded
             */

            // uint64_t tt1 = current_time_monotonic(ac->mono_time);

            rc = opus_decode(ac->decoder, msg->data + 4, msg->len - 4, ac->temp_audio_buffer, 5760, use_fec);

            // LOGGER_API_WARNING(ac->tox, "opus:DEC:time=%d", (int)(current_time_monotonic(ac->mono_time) - tt1));

// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
#if 1

            if (rc >= 0) {
                // what is the audio to video latency?
                const struct RTPHeader *header_v3 = (void *) & (msg->header);

                // LOGGER_ERROR(ac->log, "AUDIO:TTx: %llu %lld now=%llu", header_v3->frame_record_timestamp, (long long)*a_r_timestamp, current_time_monotonic(ac->mono_time));
                if (header_v3->frame_record_timestamp > 0) {
                    if (*a_r_timestamp < header_v3->frame_record_timestamp) {
                        // LOGGER_ERROR(ac->log, "AUDIO:TTx:2: %llu", header_v3->frame_record_timestamp);
                        *a_r_timestamp = header_v3->frame_record_timestamp;
                        *a_l_timestamp = current_time_monotonic(ac->mono_time);
                    } else {
                        // TODO: this should not happen here!
                        LOGGER_DEBUG(ac->log, "AUDIO: remote timestamp older");
                    }
                }

                // what is the audio to video latency?
            }

#endif
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------
// -------- DEBUG:AUDIO/VIDEO DELAY/LATENCY --------

            free(msg);
            msg = NULL;
        }

        if (rc < 0) {
            LOGGER_WARNING(ac->log, "Decoding error: %s", opus_strerror(rc));
        } else if (ac->acb) {
            ac->lp_frame_duration = (rc * 1000) / ac->lp_sampling_rate;
            ac->acb(ac->av, ac->friend_number, ac->temp_audio_buffer, rc, ac->lp_channel_count,
                    ac->lp_sampling_rate, ac->acb_user_data);
        }

        return ret_value;
    }

    pthread_mutex_unlock(ac->queue_mutex);

    return ret_value;
}

int ac_queue_message(Mono_Time *mono_time, void *acp, struct RTPMessage *msg)
{
    if (!acp || !msg) {
        if (msg) {
            free(msg);
        }

        return -1;
    }

    ACSession *ac = (ACSession *)acp;

    // LOGGER_WARNING(ac->log, "mono:%p %p %d %d",
    //            ac->mono_time,
    //            mono_time,
    //            (int)current_time_monotonic(ac->mono_time),
    //            (int)current_time_monotonic(mono_time));

    if ((msg->header.pt & 0x7f) == (RTP_TYPE_AUDIO + 2) % 128) {
        LOGGER_WARNING(ac->log, "Got dummy!");
        free(msg);
        return 0;
    }

    if ((msg->header.pt & 0x7f) != RTP_TYPE_AUDIO % 128) {
        LOGGER_WARNING(ac->log, "Invalid payload type!");
        free(msg);
        return -1;
    }

    pthread_mutex_lock(ac->queue_mutex);

    const struct RTPHeader *header_v3 = (void *) & (msg->header);
    // LOGGER_API_WARNING(ac->tox, "TT:queue:A:seqnum=%d %llu", (int)header_v3->sequnum, header_v3->frame_record_timestamp);

    // older clients do not send the frame record timestamp
    // compensate by using the frame sennt timestamp
    if (msg->header.frame_record_timestamp == 0) {
        msg->header.frame_record_timestamp = msg->header.timestamp;
    }

    jbuf_write(nullptr, ac, (struct TSBuffer *)ac->j_buf, msg);

    LOGGER_DEBUG(ac->log, "AADEBUG:OK:seqnum=%d dt=%d ts:%d curts:%d", (int)header_v3->sequnum,
                 (int)((uint64_t)header_v3->frame_record_timestamp - (uint64_t)ac->last_incoming_frame_ts),
                 (int)header_v3->frame_record_timestamp,
                 (int)current_time_monotonic(ac->mono_time));

    ac->last_incoming_frame_ts = header_v3->frame_record_timestamp;

    pthread_mutex_unlock(ac->queue_mutex);

    return 0;
}

int ac_reconfigure_encoder(ACSession *ac, int32_t bit_rate, int32_t sampling_rate, uint8_t channels)
{
    if (!ac || !reconfigure_audio_encoder(nullptr, &ac->encoder, bit_rate,
                                          sampling_rate, channels,
                                          &ac->le_bit_rate,
                                          &ac->le_sample_rate,
                                          &ac->le_channel_count)) {
        return -1;
    }

    return 0;
}

static struct TSBuffer *jbuf_new(int size)
{
    TSBuffer *res = tsb_new(size);
    return res;
}

static void jbuf_free(struct TSBuffer *q)
{
    tsb_drain(q);
    tsb_kill(q);
}

#if 0
static struct RTPMessage *new_empty_message(size_t allocate_len, const uint8_t *data, uint16_t data_length)
{
    struct RTPMessage *msg = (struct RTPMessage *)calloc(sizeof(struct RTPMessage) + (allocate_len - sizeof(
                                 struct RTPHeader)), 1);

    msg->len = data_length - sizeof(struct RTPHeader); // result without header
    memcpy(&msg->header, data, data_length);

    // clear data
    memset(&msg->data, 0, (size_t)(msg->len));

    msg->header.sequnum = net_ntohs(msg->header.sequnum);
    msg->header.timestamp = net_ntohl(msg->header.timestamp);
    msg->header.ssrc = net_ntohl(msg->header.ssrc);

    msg->header.offset_lower = net_ntohs(msg->header.offset_lower);
    msg->header.data_length_lower = net_ntohs(msg->header.data_length_lower); // result without header

    return msg;
}
#endif

static int jbuf_write(Logger *log, ACSession *ac, struct TSBuffer *q, struct RTPMessage *m)
{
    void *tmp_buf2 = tsb_write(q, (void *)m, 0, (uint32_t)m->header.frame_record_timestamp);

    if (tmp_buf2 != NULL) {
        LOGGER_DEBUG(log, "AADEBUG:rb_write: error in rb_write:rb_size=%d", (int)tsb_size(q));
        free(tmp_buf2);
        return -1;
    }

    return 0;
}

static OpusEncoder *create_audio_encoder(const Logger *log, int32_t bit_rate, int32_t sampling_rate,
        int32_t channel_count)
{
    int status = OPUS_OK;

    /*
        OPUS_APPLICATION_VOIP Process signal for improved speech intelligibility
        OPUS_APPLICATION_AUDIO Favor faithfulness to the original input
        OPUS_APPLICATION_RESTRICTED_LOWDELAY Configure the minimum possible coding delay
    */
#ifdef RPIZEROW
    // LOGGER_INFO(log, "starting audio encoder: OPUS_APPLICATION_RESTRICTED_LOWDELAY");
    // OpusEncoder *rc = opus_encoder_create(sampling_rate, channel_count, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &status);
    LOGGER_INFO(log, "starting audio encoder: OPUS_APPLICATION_VOIP");
    OpusEncoder *rc = opus_encoder_create(sampling_rate, channel_count, OPUS_APPLICATION_VOIP, &status);
#else
    LOGGER_INFO(log, "starting audio encoder: OPUS_APPLICATION_VOIP");
    OpusEncoder *rc = opus_encoder_create(sampling_rate, channel_count, OPUS_APPLICATION_VOIP, &status);
#endif

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while starting audio encoder: %s", opus_strerror(status));
        return nullptr;
    } else {
        LOGGER_INFO(log, "starting audio encoder OK: %s", opus_strerror(status));
    }

    /*
     * Rates from 500 to 512000 bits per second are meaningful as well as the special
     * values OPUS_BITRATE_AUTO and OPUS_BITRATE_MAX. The value OPUS_BITRATE_MAX can
     * be used to cause the codec to use as much rate as it can, which is useful for
     * controlling the rate by adjusting the output buffer size.
     *
     * Parameters:
     *   `[in]`    `x`   `opus_int32`: bitrate in bits per second.
     */
    status = opus_encoder_ctl(rc, OPUS_SET_BITRATE(bit_rate));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }


    /*
     * Configures the encoder's use of inband forward error correction.
     * Note:
     *   This is only applicable to the LPC layer
     * Parameters:
     *   `[in]`    `x`   `int`: FEC flag, 0 (disabled) is default
     */
    /* Enable in-band forward error correction in codec */
    status = opus_encoder_ctl(rc, OPUS_SET_INBAND_FEC(1));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }


    /*
     * Configures the encoder's expected packet loss percentage.
     * Higher values with trigger progressively more loss resistant behavior in
     * the encoder at the expense of quality at a given bitrate in the lossless case,
     * but greater quality under loss.
     * Parameters:
     *     `[in]`    `x`   `int`: Loss percentage in the range 0-100, inclusive.
     */
    /* Make codec resistant to up to x% packet loss
     * NOTE This could also be adjusted on the fly, rather than hard-coded,
     *      with feedback from the receiving client.
     */
    status = opus_encoder_ctl(rc, OPUS_SET_PACKET_LOSS_PERC(AUDIO_OPUS_PACKET_LOSS_PERC));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

#if 0
    status = opus_encoder_ctl(rc, OPUS_SET_VBR(0));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

#endif

    /*
     * Configures the encoder's computational complexity.
     *
     * The supported range is 0-10 inclusive with 10 representing the highest complexity.
     * The default value is 10.
     *
     * Parameters:
     *   `[in]`    `x`   `int`: 0-10, inclusive
     */
    /* Set algorithm to the highest complexity, maximizing compression */
    LOGGER_INFO(log, "starting audio encoder complexity: %d", (int)AUDIO_OPUS_COMPLEXITY);
    status = opus_encoder_ctl(rc, OPUS_SET_COMPLEXITY(AUDIO_OPUS_COMPLEXITY));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

    return rc;

FAILURE:
    opus_encoder_destroy(rc);
    return nullptr;
}

static bool reconfigure_audio_encoder(const Logger *log, OpusEncoder **e, int32_t new_br, int32_t new_sr,
                                      uint8_t new_ch, int32_t *old_br, int32_t *old_sr, int32_t *old_ch)
{
    /* Values are checked in toxav.c */
    if (*old_sr != new_sr || *old_ch != new_ch) {
        OpusEncoder *new_encoder = create_audio_encoder(log, new_br, new_sr, new_ch);

        if (new_encoder == nullptr) {
            return false;
        }

        opus_encoder_destroy(*e);
        *e = new_encoder;
    } else if (*old_br == new_br) {
        return true; /* Nothing changed */
    }

    int status = opus_encoder_ctl(*e, OPUS_SET_BITRATE(new_br));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        return false;
    }

    *old_br = new_br;
    *old_sr = new_sr;
    *old_ch = new_ch;

    LOGGER_DEBUG(log, "Reconfigured audio encoder br: %d sr: %d cc:%d", new_br, new_sr, new_ch);
    return true;
}

static bool reconfigure_audio_decoder(ACSession *ac, int32_t sampling_rate, int8_t channels)
{
    if (sampling_rate <= 0) {
        return false;
    }

    if (sampling_rate != ac->ld_sample_rate || channels != ac->ld_channel_count) {
        if (current_time_monotonic(ac->mono_time) - ac->ldrts < 500) {
            return false;
        }

        int status;
        OpusDecoder *new_dec = opus_decoder_create(sampling_rate, channels, &status);

        if (status != OPUS_OK) {
            LOGGER_ERROR(ac->log, "Error while starting audio decoder(%d %d): %s", sampling_rate, channels, opus_strerror(status));
            return false;
        }

        ac->ld_sample_rate = sampling_rate;
        ac->ld_channel_count = channels;
        ac->ldrts = current_time_monotonic(ac->mono_time);

        opus_decoder_destroy(ac->decoder);
        ac->decoder = new_dec;

        LOGGER_DEBUG(ac->log, "Reconfigured audio decoder sr: %d cc: %d", sampling_rate, channels);
    }

    return true;
}


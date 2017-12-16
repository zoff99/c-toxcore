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

#include "audio.h"

#include "rtp.h"

#include "../toxcore/logger.h"

#include <stdlib.h>


struct JitterBuffer {
    struct RTPMessage **queue;
    uint32_t size;
    uint32_t capacity;
    uint16_t bottom;
    uint16_t top;
    uint32_t elements_inside;
};


static struct JitterBuffer *jbuf_new(uint32_t capacity);
static void jbuf_clear(struct JitterBuffer *q);
static void jbuf_free(struct JitterBuffer *q);
static int jbuf_write(Logger *log, struct JitterBuffer *q, struct RTPMessage *m);
OpusEncoder *create_audio_encoder(Logger *log, int32_t bit_rate, int32_t sampling_rate, int32_t channel_count);
bool reconfigure_audio_encoder(Logger *log, OpusEncoder **e, int32_t new_br, int32_t new_sr, uint8_t new_ch,
                               int32_t *old_br, int32_t *old_sr, int32_t *old_ch);
bool reconfigure_audio_decoder(ACSession *ac, int32_t sampling_rate, int8_t channels);



ACSession *ac_new(Logger *log, ToxAV *av, uint32_t friend_number, toxav_audio_receive_frame_cb *cb, void *cb_data)
{
    ACSession *ac = (ACSession *)calloc(sizeof(ACSession), 1);

    if (!ac) {
        LOGGER_WARNING(log, "Allocation failed! Application might misbehave!");
        return NULL;
    }

    if (create_recursive_mutex(ac->queue_mutex) != 0) {
        LOGGER_WARNING(log, "Failed to create recursive mutex!");
        free(ac);
        return NULL;
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

    ac->log = log;

    /* Initialize encoders with default values */
    ac->encoder = create_audio_encoder(log, AUDIO_START_BITRATE_RATE, AUDIO_START_SAMPLING_RATE, AUDIO_START_CHANNEL_COUNT);

    if (ac->encoder == NULL) {
        goto DECODER_CLEANUP;
    }

    ac->le_bit_rate = AUDIO_START_BITRATE_RATE;
    ac->le_sample_rate = AUDIO_START_SAMPLING_RATE;
    ac->le_channel_count = AUDIO_START_CHANNEL_COUNT;

    ac->ld_channel_count = AUDIO_DECODER__START_SAMPLING_RATE;
    ac->ld_sample_rate = AUDIO_DECODER__START_CHANNEL_COUNT;
    ac->ldrts = 0; /* Make it possible to reconfigure straight away */

    /* These need to be set in order to properly
     * do error correction with opus */
    ac->lp_frame_duration = AUDIO_MAX_FRAME_DURATION_MS;
    ac->lp_sampling_rate = AUDIO_DECODER__START_SAMPLING_RATE;
    ac->lp_channel_count = AUDIO_DECODER__START_CHANNEL_COUNT;

    ac->av = av;
    ac->friend_number = friend_number;
    ac->acb.first = cb;
    ac->acb.second = cb_data;

    return ac;

DECODER_CLEANUP:
    opus_decoder_destroy(ac->decoder);
    jbuf_free((struct JitterBuffer *)ac->j_buf);
BASE_CLEANUP:
    pthread_mutex_destroy(ac->queue_mutex);
    free(ac);
    return NULL;
}

void ac_kill(ACSession *ac)
{
    if (!ac) {
        return;
    }

    opus_encoder_destroy(ac->encoder);
    opus_decoder_destroy(ac->decoder);
    jbuf_free((struct JitterBuffer *)ac->j_buf);

    pthread_mutex_destroy(ac->queue_mutex);

    LOGGER_DEBUG(ac->log, "Terminated audio handler: %p", ac);
    free(ac);
}

static inline struct RTPMessage *jbuf_read(Logger *log, struct JitterBuffer *q, int32_t *success)
{
    if ((q->top % q->size) == (q->bottom % q->size)) {
        *success = 0;
        return NULL;
    }

    unsigned int num = q->bottom % q->size;

    if (q->queue[num]) {
        struct RTPMessage *ret = q->queue[num];
        q->queue[num] = NULL;
        ++q->bottom;
        if (q->elements_inside > 0)
        {
            q->elements_inside--;
        }
        *success = 1;
        
        if (q->bottom > q->size)
        {
            q->bottom = q->bottom % q->size;
        }
        
        return ret;
    }

    if ((uint32_t)(q->top - q->bottom) > q->capacity) {
        LOGGER_DEBUG(log, "drop oldest incoming audio frame");
        ++q->bottom;
        if (q->elements_inside > 0)
        {
            q->elements_inside--;
        }

        if (q->bottom > q->size)
        {
            q->bottom = q->bottom % q->size;
        }

        *success = 2;
        return NULL;
    }

    *success = 0;
    return NULL;
}

static inline uint8_t jbuf_is_empty(struct JitterBuffer *q)
{
    if (q->elements_inside < 1)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

uint8_t ac_iterate(ACSession *ac)
{
    if (!ac) {
        return 0;
    }

    uint8_t ret_value = 1;
    struct JitterBuffer *jbuffer = (struct JitterBuffer *)ac->j_buf;

    LOGGER_INFO(ac->log, "jitterbuffer elements=%u", jbuffer->elements_inside);

    if (jbuf_is_empty(jbuffer) == 1)
    {
        return 0;
    }
    else if (jbuffer->elements_inside > AUDIO_JITTERBUFFER_FILL_THRESHOLD)
    {
        // audio frames are building up, skip video frames to compensate
        LOGGER_INFO(ac->log, "incoming audio frames are slowing down");
        ret_value = 2;
    }
#if 0
    else if (jbuffer->elements_inside > AUDIO_JITTERBUFFER_SKIP_THRESHOLD)
    {
        // audio frames are still building up, skip audio frames to synchronize again
        int rc_skip = 0;
        LOGGER_WARNING(ac->log, "skipping some incoming audio frames");
        jbuf_read(ac->log, jbuffer, &rc_skip);
        jbuf_read(ac->log, jbuffer, &rc_skip);
        jbuf_read(ac->log, jbuffer, &rc_skip);
        jbuf_read(ac->log, jbuffer, &rc_skip);
        jbuf_read(ac->log, jbuffer, &rc_skip);
        jbuf_read(ac->log, jbuffer, &rc_skip);
        return 1;
    }
#endif

    /* TODO(mannol): fix this and jitter buffering */

    /* Enough space for the maximum frame size (120 ms 48 KHz stereo audio) */
    int16_t temp_audio_buffer[AUDIO_MAX_BUFFER_SIZE_PCM16_FOR_FRAME_PER_CHANNEL *
                              AUDIO_MAX_CHANNEL_COUNT];

    struct RTPMessage *msg;
    int rc = 0;

    pthread_mutex_lock(ac->queue_mutex);

    while ((msg = jbuf_read(ac->log, jbuffer, &rc)) || rc == 2) {
        pthread_mutex_unlock(ac->queue_mutex);

        if (rc == 2) {
            LOGGER_DEBUG(ac->log, "OPUS correction");
            int fs = (ac->lp_sampling_rate * ac->lp_frame_duration) / 1000;
            rc = opus_decode(ac->decoder, NULL, 0, temp_audio_buffer, fs, 1);
        } else {
            /* Get values from packet and decode. */
            /* NOTE: This didn't work very well */
#if 0
            rc = convert_bw_to_sampling_rate(opus_packet_get_bandwidth(msg->data));

            if (rc != -1) {
                cs->last_packet_sampling_rate = rc;
            } else {
                LOGGER_WARNING(ac->log, "Failed to load packet values!");
                rtp_free_msg(msg);
                continue;
            }

#endif


            /* Pick up sampling rate from packet */
            memcpy(&ac->lp_sampling_rate, msg->data, 4);
            ac->lp_sampling_rate = net_ntohl(ac->lp_sampling_rate);

            ac->lp_channel_count = opus_packet_get_nb_channels(msg->data + 4);

            /** NOTE: even though OPUS supports decoding mono frames with stereo decoder and vice versa,
              * it didn't work quite well.
              */
            if (!reconfigure_audio_decoder(ac, ac->lp_sampling_rate, ac->lp_channel_count)) {
                LOGGER_WARNING(ac->log, "Failed to reconfigure decoder!");
                free(msg);
                continue;
            }

            /*
            frame_size = opus_decode(dec, packet, len, decoded, max_size, 0);
              where
            packet is the byte array containing the compressed data
            len is the exact number of bytes contained in the packet
            decoded is the decoded audio data in opus_int16 (or float for opus_decode_float())
            max_size is the max duration of the frame in samples (per channel) that can fit
            into the decoded_frame array
             */
            rc = opus_decode(ac->decoder, msg->data + 4, msg->len - 4, temp_audio_buffer, 5760, 0);
            free(msg);
        }

        if (rc < 0) {
            LOGGER_WARNING(ac->log, "Decoding error: %s", opus_strerror(rc));
        } else if (ac->acb.first) {
            ac->lp_frame_duration = (rc * 1000) / ac->lp_sampling_rate;

            ac->acb.first(ac->av, ac->friend_number, temp_audio_buffer, rc, ac->lp_channel_count,
                          ac->lp_sampling_rate, ac->acb.second);
        }

        return ret_value;
    }

    pthread_mutex_unlock(ac->queue_mutex);
    
    return ret_value;
}

int ac_queue_message(void *acp, struct RTPMessage *msg)
{
    if (!acp || !msg) {
        return -1;
    }

    ACSession *ac = (ACSession *)acp;

    if ((msg->header.pt & 0x7f) == (rtp_TypeAudio + 2) % 128) {
        LOGGER_WARNING(ac->log, "Got dummy!");
        free(msg);
        return 0;
    }

    if ((msg->header.pt & 0x7f) != rtp_TypeAudio % 128) {
        LOGGER_WARNING(ac->log, "Invalid payload type!");
        free(msg);
        return -1;
    }

    pthread_mutex_lock(ac->queue_mutex);
    int rc = jbuf_write(ac->log, (struct JitterBuffer *)ac->j_buf, msg);
    pthread_mutex_unlock(ac->queue_mutex);

    if (rc == -1) {
        LOGGER_WARNING(ac->log, "Could not queue the incoming audio message!");
        free(msg);
        return -1;
    }

    return 0;
}

int ac_reconfigure_encoder(ACSession *ac, int32_t bit_rate, int32_t sampling_rate, uint8_t channels)
{
    if (!ac || !reconfigure_audio_encoder(ac->log, &ac->encoder, bit_rate,
                                          sampling_rate, channels,
                                          &ac->le_bit_rate,
                                          &ac->le_sample_rate,
                                          &ac->le_channel_count)) {
        return -1;
    }

    return 0;
}

static struct JitterBuffer *jbuf_new(uint32_t capacity)
{
    unsigned int size = 1;

    while (size <= (capacity * 4)) {
        size *= 2;
    }

    struct JitterBuffer *q = (struct JitterBuffer *)calloc(sizeof(struct JitterBuffer), 1);

    if (!q) {
        return NULL;
    }

    q->queue = (struct RTPMessage **)calloc(sizeof(struct RTPMessage *), size);

    if (!q->queue) {
        free(q);
        return NULL;
    }

    q->size = size;
    q->capacity = capacity;
    q->elements_inside = 0;
    return q;
}

static void jbuf_clear(struct JitterBuffer *q)
{
    for (; q->bottom != q->top; ++q->bottom) {
        if (q->queue[q->bottom % q->size]) {
            free(q->queue[q->bottom % q->size]);
            q->queue[q->bottom % q->size] = NULL;
        }
    }
    q->elements_inside = 0;
}

static void jbuf_free(struct JitterBuffer *q)
{
    if (!q) {
        return;
    }

    jbuf_clear(q);
    free(q->queue);
    free(q);
}


static int jbuf_write(Logger *log, struct JitterBuffer *q, struct RTPMessage *m)
{
    // uint16_t sequnum = m->header.sequnum;

    unsigned int num = q->top % q->size;

    if (q->queue[num]) {
        LOGGER_WARNING(log, "jitter buffer full: %p", q);
        return -1;
    }

    q->queue[num] = m;
    q->elements_inside++;

    //if ((sequnum - q->bottom) >= (q->top - q->bottom)) {
    //    q->top = sequnum + 1;
    //}
    q->top++;
    if (q->top > q->size)
    {
        q->top = q->top % q->size;
    }

    return 0;
}


static int jbuf_write__OLD(Logger *log, struct JitterBuffer *q, struct RTPMessage *m)
{
    uint16_t sequnum = m->header.sequnum;

    unsigned int num = sequnum % q->size;

    if ((uint32_t)(sequnum - q->bottom) > q->size) {
        LOGGER_WARNING(log, "Clearing filled jitter buffer: %p", q);

        jbuf_clear(q);
        q->bottom = sequnum - q->capacity;
        q->queue[num] = m;
        q->elements_inside++;
        q->top = sequnum + 1;
        return 0;
    }

    if (q->queue[num]) {
        LOGGER_WARNING(log, "jitter buffer full: %p", q);
        return -1;
    }

    q->queue[num] = m;
    q->elements_inside++;

    if ((sequnum - q->bottom) >= (q->top - q->bottom)) {
        q->top = sequnum + 1;
    }

    return 0;
}

OpusEncoder *create_audio_encoder(Logger *log, int32_t bit_rate, int32_t sampling_rate, int32_t channel_count)
{
    int status = OPUS_OK;

    /*
        OPUS_APPLICATION_VOIP Process signal for improved speech intelligibility
        OPUS_APPLICATION_AUDIO Favor faithfulness to the original input
        OPUS_APPLICATION_RESTRICTED_LOWDELAY Configure the minimum possible coding delay
    */
    OpusEncoder *rc = opus_encoder_create(sampling_rate, channel_count, OPUS_APPLICATION_VOIP, &status);

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while starting audio encoder: %s", opus_strerror(status));
        return NULL;
    }

    /*
      Rates from 500 to 512000 bits per second are meaningful as well as the special
      values OPUS_BITRATE_AUTO and OPUS_BITRATE_MAX. The value OPUS_BITRATE_MAX can
      be used to cause the codec to use as much rate as it can, which is useful for
      controlling the rate by adjusting the output buffer size.

      Parameters:
        [in]    x   opus_int32: bitrate in bits per second.
    */
    status = opus_encoder_ctl(rc, OPUS_SET_BITRATE(bit_rate));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }


    /*
      Configures the encoder's use of inband forward error correction.
      Note:
        This is only applicable to the LPC layer
      Parameters:
        [in]    x   int: FEC flag, 0 (disabled) is default
     */
    /* Enable in-band forward error correction in codec */
    status = opus_encoder_ctl(rc, OPUS_SET_INBAND_FEC(1));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

    /*
    Configures the encoder's expected packet loss percentage.
    Higher values with trigger progressively more loss resistant behavior in
    the encoder at the expense of quality at a given bitrate in the lossless case,
    but greater quality under loss.
    Parameters:
        [in]    x   int: Loss percentage in the range 0-100, inclusive.
     */
    /* Make codec resistant to up to 10% packet loss
     * NOTE This could also be adjusted on the fly, rather than hard-coded,
     *      with feedback from the receiving client.
     */
    status = opus_encoder_ctl(rc, OPUS_SET_PACKET_LOSS_PERC(AUDIO_OPUS_PACKET_LOSS_PERC));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

    /*
      Configures the encoder's computational complexity.

      The supported range is 0-10 inclusive with 10 representing the highest complexity.
      The default value is 10.

      Parameters:
        [in]    x   int: 0-10, inclusive
     */
    /* Set algorithm to the highest complexity, maximizing compression */
    status = opus_encoder_ctl(rc, OPUS_SET_COMPLEXITY(AUDIO_OPUS_COMPLEXITY));

    if (status != OPUS_OK) {
        LOGGER_ERROR(log, "Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }

    return rc;

FAILURE:
    opus_encoder_destroy(rc);
    return NULL;
}

bool reconfigure_audio_encoder(Logger *log, OpusEncoder **e, int32_t new_br, int32_t new_sr, uint8_t new_ch,
                               int32_t *old_br, int32_t *old_sr, int32_t *old_ch)
{
    /* Values are checked in toxav.c */
    if (*old_sr != new_sr || *old_ch != new_ch) {
        OpusEncoder *new_encoder = create_audio_encoder(log, new_br, new_sr, new_ch);

        if (new_encoder == NULL) {
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

bool reconfigure_audio_decoder(ACSession *ac, int32_t sampling_rate, int8_t channels)
{
    if (sampling_rate != ac->ld_sample_rate || channels != ac->ld_channel_count) {
        if (current_time_monotonic() - ac->ldrts < 500) {
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
        ac->ldrts = current_time_monotonic();

        opus_decoder_destroy(ac->decoder);
        ac->decoder = new_dec;

        LOGGER_DEBUG(ac->log, "Reconfigured audio decoder sr: %d cc: %d", sampling_rate, channels);
    }

    return true;
}


/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifndef C_TOXCORE_TOXAV_AUDIO_H
#define C_TOXCORE_TOXAV_AUDIO_H

#include "toxav.h"
#include "video.h"

#include "../toxcore/logger.h"
#include "../toxcore/util.h"
#include "rtp.h"

#include <opus.h>
#include <pthread.h>


#define AUDIO_JITTERBUFFER_COUNT 500 // ~ (5ms * AUDIO_JITTERBUFFER_COUNT) --> to hold that many ms audio data for audio-to-video delay
#define AUDIO_JITTERBUFFER_FILL_THRESHOLD (98) // this should be lower than the above value!
#define AUDIO_JITTERBUFFER_SKIP_THRESHOLD (99)

#define AUDIO_JITTERBUFFER_MIN_FILLED (0)

#define AUDIO_MAX_SAMPLING_RATE (48000)
#define AUDIO_MAX_CHANNEL_COUNT (2)

#define AUDIO_START_SAMPLING_RATE (48000)
#define AUDIO_START_BITRATE_RATE (48000)
#define AUDIO_START_CHANNEL_COUNT (2)
#define AUDIO_OPUS_PACKET_LOSS_PERC (2) // allow upto XX % loss of audio packets

#ifdef RPIZEROW
#define AUDIO_OPUS_COMPLEXITY (0)
#else
#define AUDIO_OPUS_COMPLEXITY (10)
#endif

#define AUDIO_DECODER__START_SAMPLING_RATE (48000)
#define AUDIO_DECODER__START_CHANNEL_COUNT (2)

#define AUDIO_MAX_FRAME_DURATION_MS (120)

#define AUDIO_LOST_FRAME_INDICATOR (4)

// ((sampling_rate_in_hz * frame_duration_in_ms) / 1000) * 2 // because PCM16 needs 2 bytes for 1 sample
#define AUDIO_MAX_BUFFER_SIZE_PCM16_FOR_FRAME_PER_CHANNEL ((AUDIO_MAX_SAMPLING_RATE * AUDIO_MAX_FRAME_DURATION_MS) / 1000)
#define AUDIO_MAX_BUFFER_SIZE_BYTES_FOR_FRAME_PER_CHANNEL (AUDIO_MAX_BUFFER_SIZE_PCM16_FOR_FRAME_PER_CHANNEL * 2)

/* debugging */
// #define AUDIO_DEBUGGING_SKIP_FRAMES 1
// #define AUDIO_DEBUGGING_SIMULATE_SOME_DATA_LOSS 1
/* debugging */

typedef struct ACSession_s {
    Mono_Time *mono_time;

    /* encoding */
    OpusEncoder *encoder;
    int32_t le_sample_rate; /* Last encoder sample rate */
    int32_t le_channel_count; /* Last encoder channel count */
    int32_t le_bit_rate; /* Last encoder bit rate */

    /* decoding */
    OpusDecoder *decoder;
    int32_t lp_channel_count; /* Last packet channel count */
    int32_t lp_sampling_rate; /* Last packet sample rate */
    int32_t lp_frame_duration; /* Last packet frame duration */
    int32_t ld_sample_rate; /* Last decoder sample rate */
    int32_t ld_channel_count; /* Last decoder channel count */
    uint64_t ldrts; /* Last decoder reconfiguration time stamp */
    int32_t lp_seqnum_new; /* last incoming packet sequence number */
    void *j_buf; /* it's a Ringbuffer now */
    int16_t temp_audio_buffer[AUDIO_MAX_BUFFER_SIZE_PCM16_FOR_FRAME_PER_CHANNEL *
                                                                                AUDIO_MAX_CHANNEL_COUNT];

    int64_t timestamp_difference_to_sender;
    uint64_t last_incoming_frame_ts;
    uint8_t encoder_frame_has_record_timestamp;
    uint32_t audio_received_first_frame;

    pthread_mutex_t queue_mutex[1];

    ToxAV *av;
    Tox *tox;
    uint32_t friend_number;
    /* Audio frame receive callback */
    toxav_audio_receive_frame_cb *acb;
    void *acb_user_data;

    toxav_audio_receive_frame_pts_cb *acb_pts;
    void *acb_pts_user_data;
} ACSession;

ACSession *ac_new(Mono_Time *mono_time, const Logger *log, ToxAV *av, Tox *tox, uint32_t friend_number,
                  toxav_audio_receive_frame_cb *cb, void *cb_data,
                  toxav_audio_receive_frame_pts_cb *cb_pts, void *cb_pts_data);
void ac_kill(ACSession *ac);
uint8_t ac_iterate(ACSession *ac, uint64_t *a_r_timestamp, uint64_t *a_l_timestamp, uint64_t *v_r_timestamp,
                   uint64_t *v_l_timestamp,
                   int64_t *timestamp_difference_adjustment_,
                   int64_t *timestamp_difference_to_sender_,
                   int video_send_cap,
                   int32_t *video_has_rountrip_time_ms);
int ac_queue_message(Mono_Time *mono_time, void *acp, struct RTPMessage *msg);
int ac_reconfigure_encoder(ACSession *ac, int32_t bit_rate, int32_t sampling_rate, uint8_t channels);

#endif // C_TOXCORE_TOXAV_AUDIO_H

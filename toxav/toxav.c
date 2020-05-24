/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "toxav.h"
#include "../toxcore/tox.h"

#include "msi.h"
#include "rtp.h"
#include "video.h"


#include "../toxcore/logger.h"
#include "../toxcore/util.h"
#include "../toxcore/mono_time.h"


#include "tox_generic.h"

#include "codecs/toxav_codecs.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>


#if defined(AUDIO_DEBUGGING_SKIP_FRAMES)
uint32_t _debug_count_sent_audio_frames = 0;
uint32_t _debug_skip_every_x_audio_frame = 10;
#endif

#define AUDIO_ITERATATIONS_WHILE_VIDEO (5)
#define VIDEO_MIN_SEND_KEYFRAME_INTERVAL 6000

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

void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data);

static int callback_invite(void *toxav_inst, MSICall *call);
static int callback_start(void *toxav_inst, MSICall *call);
static int callback_end(void *toxav_inst, MSICall *call);
static int callback_error(void *toxav_inst, MSICall *call);
static int callback_capabilites(void *toxav_inst, MSICall *call);

static bool audio_bit_rate_invalid(uint32_t bit_rate);
static bool video_bit_rate_invalid(uint32_t bit_rate);
static bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state);
static ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, TOXAV_ERR_CALL *error);
static ToxAVCall *call_remove(ToxAVCall *call);
static bool call_prepare_transmission(ToxAVCall *call);
static void call_kill_transmission(ToxAVCall *call);

MSISession *tox_av_msi_get(ToxAV *av);
int toxav_friend_exists(const Tox *tox, int32_t friendnumber);
Mono_Time *toxav_get_av_mono_time(ToxAV *toxav);
ToxAVCall *call_get(ToxAV *av, uint32_t friend_number);
RTPSession *rtp_session_get(void *call, int payload_type);
BWController *bwc_controller_get(void *call);


MSISession *tox_av_msi_get(ToxAV *av)
{
    if (!av) {
        return nullptr;
    }

    return av->msi;
}

ToxAVCall *call_get(ToxAV *av, uint32_t friend_number)
{
    if (av == nullptr) {
        return nullptr;
    }

    /* Assumes mutex locked */
    if (av->calls == nullptr || av->calls_tail < friend_number) {
        return nullptr;
    }

    return av->calls[friend_number];
}

RTPSession *rtp_session_get(void *call, int payload_type)
{
    if (((ToxAVCall *)call) == nullptr) {
        return nullptr;
    }

    if (payload_type == RTP_TYPE_VIDEO) {
        return ((ToxAVCall *)call)->video_rtp;
    } else if (payload_type == RTP_TYPE_AUDIO) {
        return ((ToxAVCall *)call)->audio_rtp;
    }

    return nullptr;
}

BWController *bwc_controller_get(void *call)
{
    if (((ToxAVCall *)call) == nullptr) {
        return nullptr;
    }

    return ((ToxAVCall *)call)->bwc;
}

ToxAV *toxav_new(Tox *tox, Toxav_Err_New *error)
{
    Toxav_Err_New rc = TOXAV_ERR_NEW_OK;
    ToxAV *av = nullptr;

    if (tox == nullptr) {
        rc = TOXAV_ERR_NEW_NULL;
        goto RETURN;
    }

    av = (ToxAV *)calloc(sizeof(ToxAV), 1);

    if (av == nullptr) {
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    if (create_recursive_mutex(av->mutex) != 0) {
        LOGGER_WARNING(m->log, "Mutex creation failed!");
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    av->tox = tox;
    av->msi = msi_new(av->tox);

    rtp_allow_receiving(av->tox);
    bwc_allow_receiving(av->tox);

    av->toxav_mono_time = mono_time_new();

    if (av->msi == nullptr) {
        pthread_mutex_destroy(av->mutex);
        rc = TOXAV_ERR_NEW_MALLOC;
        goto RETURN;
    }

    av->interval = 200;
    av->msi->av = av;

    // save Tox object into toxcore
    tox_set_av_object(av->tox, (void *)av);

    msi_register_callback(av->msi, callback_invite, MSI_ON_INVITE);
    msi_register_callback(av->msi, callback_start, MSI_ON_START);
    msi_register_callback(av->msi, callback_end, MSI_ON_END);
    msi_register_callback(av->msi, callback_error, MSI_ON_ERROR);
    msi_register_callback(av->msi, callback_error, MSI_ON_PEERTIMEOUT);
    msi_register_callback(av->msi, callback_capabilites, MSI_ON_CAPABILITIES);

RETURN:

    if (error) {
        *error = rc;
    }

    if (rc != TOXAV_ERR_NEW_OK) {
        free(av);
        av = NULL;
    }

    return av;
}

void toxav_kill(ToxAV *av)
{
    if (av == nullptr) {
        return;
    }

    pthread_mutex_lock(av->mutex);

    // unregister callbacks
    for (uint8_t i = PACKET_ID_RANGE_LOSSY_AV_START; i <= PACKET_ID_RANGE_LOSSY_AV_END; ++i) {
        tox_callback_friend_lossy_packet_per_pktid(av->tox, nullptr, i);
    }

    rtp_stop_receiving(av->tox);
    bwc_stop_receiving(av->tox);

    /* To avoid possible deadlocks */
    while (av->msi && msi_kill(av->tox, av->msi, nullptr) != 0) {
        pthread_mutex_unlock(av->mutex);
        pthread_mutex_lock(av->mutex);
    }

    /* Msi kill will hang up all calls so just clean these calls */
    if (av->calls) {
        ToxAVCall *it = call_get(av, av->calls_head);

        while (it) {
            call_kill_transmission(it);
            it->msi_call = nullptr; /* msi_kill() frees the call's msi_call handle; which causes #278 */
            it = call_remove(it); /* This will eventually free av->calls */
        }
    }

    mono_time_free(av->toxav_mono_time);

    pthread_mutex_unlock(av->mutex);
    pthread_mutex_destroy(av->mutex);

    free(av);
}

Tox *toxav_get_tox(const ToxAV *av)
{
    return av->tox;
}

uint32_t toxav_iteration_interval(const ToxAV *av)
{
    /* If no call is active interval is 200 */
    return av->calls ? av->interval : 200;
}

void toxav_audio_iterate_seperation(ToxAV *av, bool active)
{
    if (av) {
        pthread_mutex_lock(av->mutex);
        av->toxav_audio_iterate_seperation_active = active;
        pthread_mutex_unlock(av->mutex);
    }
}

void toxav_audio_iterate(ToxAV *av)
{
    pthread_mutex_lock(av->mutex);

    if (av->calls == nullptr) {
        pthread_mutex_unlock(av->mutex);
        return;
    }

    // TODO: this works, but is not future proof
    uint32_t num_friends = (uint32_t)tox_self_get_friend_list_size(av->tox);

    for (uint32_t fid = 0; fid < num_friends; ++fid) {

        ToxAVCall *i = call_get(av,  fid);

        if (i) {
            if (i->active) {

                pthread_mutex_unlock(av->mutex);
                pthread_mutex_lock(i->toxav_call_mutex);
                int64_t copy_of_value = i->call_timestamp_difference_to_sender;
                int video_cap_copy = (int)(i->msi_call->self_capabilities & MSI_CAP_S_VIDEO);


                uint8_t res_ac = ac_iterate(i->audio,
                                            &(i->last_incoming_audio_frame_rtimestamp),
                                            &(i->last_incoming_audio_frame_ltimestamp),
                                            &(i->last_incoming_video_frame_rtimestamp),
                                            &(i->last_incoming_video_frame_ltimestamp),
                                            &(i->call_timestamp_difference_adjustment),
                                            &(copy_of_value),
                                            video_cap_copy
                                           );
                pthread_mutex_unlock(i->toxav_call_mutex);
                pthread_mutex_lock(av->mutex);
            }
        }
    }

    pthread_mutex_unlock(av->mutex);
}

void toxav_iterate(ToxAV *av)
{
    pthread_mutex_lock(av->mutex);

    if (av->calls == nullptr) {
        pthread_mutex_unlock(av->mutex);
        return;
    }

    uint64_t start = current_time_monotonic(av->toxav_mono_time);
    int32_t rc = 500;
    uint32_t audio_iterations = 0;

    ToxAVCall *i = av->calls[av->calls_head];

    for (; i; i = i->next) {

        audio_iterations = 0;

        if (i->active) {

            bool audio_iterate_seperation_active = av->toxav_audio_iterate_seperation_active;

            pthread_mutex_lock(i->toxav_call_mutex);
            pthread_mutex_unlock(av->mutex);

            uint32_t fid = i->friend_number;
            bool is_offline = check_peer_offline_status(av->tox, i->msi_call->session, fid);

            if (is_offline) {
                pthread_mutex_unlock(i->toxav_call_mutex);
                pthread_mutex_lock(av->mutex);
                break;
            }

            // ------- ac_iterate for audio -------
            if (!audio_iterate_seperation_active) {
                uint8_t res_ac = ac_iterate(i->audio,
                                            &(i->last_incoming_audio_frame_rtimestamp),
                                            &(i->last_incoming_audio_frame_ltimestamp),
                                            &(i->last_incoming_video_frame_rtimestamp),
                                            &(i->last_incoming_video_frame_ltimestamp),
                                            &(i->call_timestamp_difference_adjustment),
                                            &(i->call_timestamp_difference_to_sender),
                                            (int)(i->msi_call->self_capabilities & MSI_CAP_S_VIDEO)
                                           );

                i->skip_video_flag = 0;
            }

            // ------- ac_iterate for audio -------

            // ------- av_iterate for VIDEO -------
            uint8_t got_video_frame = vc_iterate(i->video, i->av->tox, i->skip_video_flag,
                                                 &(i->last_incoming_audio_frame_rtimestamp),
                                                 &(i->last_incoming_audio_frame_ltimestamp),
                                                 &(i->last_incoming_video_frame_rtimestamp),
                                                 &(i->last_incoming_video_frame_ltimestamp),
                                                 i->bwc,
                                                 &(i->call_timestamp_difference_adjustment),
                                                 &(i->call_timestamp_difference_to_sender)
                                                );
            // ------- av_iterate for VIDEO -------


#define MIN(a,b) (((a)<(b))?(a):(b))

            if (i->msi_call->self_capabilities & MSI_CAP_R_AUDIO &&
                    i->msi_call->peer_capabilities & MSI_CAP_S_AUDIO) {
                rc = MIN((i->audio->lp_frame_duration - 4), rc);
            }

            if (i->msi_call->self_capabilities & MSI_CAP_R_VIDEO &&
                    i->msi_call->peer_capabilities & MSI_CAP_S_VIDEO) {

                pthread_mutex_lock(i->video->queue_mutex);
                rc = MIN(i->video->lcfd, (uint32_t) rc);
                pthread_mutex_unlock(i->video->queue_mutex);

            }

            pthread_mutex_unlock(i->toxav_call_mutex);
            pthread_mutex_lock(av->mutex);

            /* In case this call is popped from container stop iteration */
            if (call_get(av, fid) != i) {
                break;
            }

        }
    }

    av->interval = rc < av->dmssa ? 0 : (rc - av->dmssa);
    av->dmsst += current_time_monotonic(av->toxav_mono_time) - start;

    if (++av->dmssc == 3) {
        av->dmssa = av->dmsst / 3 + 5; /* NOTE Magic Offset 5 for precision */
        av->dmssc = 0;
        av->dmsst = 0;
    }

    pthread_mutex_unlock(av->mutex);
}

bool toxav_call(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                Toxav_Err_Call *error)
{
    Toxav_Err_Call rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call;

    pthread_mutex_lock(av->mutex);

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))) {
        rc = TOXAV_ERR_CALL_INVALID_BIT_RATE;
        goto RETURN;
    }

    call = call_new(av, friend_number, &rc);

    if (call == nullptr) {
        goto RETURN;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;
    call->video_bit_rate_not_yet_set = call->video_bit_rate;

    LOGGER_ERROR(av->m->log, "toxav_call:vb=%d", (int)video_bit_rate);

    call->previous_self_capabilities = MSI_CAP_R_AUDIO | MSI_CAP_R_VIDEO;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? MSI_CAP_S_AUDIO : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? MSI_CAP_S_VIDEO : 0;

    if (msi_invite(av->msi, &call->msi_call, friend_number, call->previous_self_capabilities) != 0) {
        call_remove(call);
        rc = TOXAV_ERR_CALL_SYNC;
        goto RETURN;
    }

    call->msi_call->av_call = call;

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_OK;
}

void toxav_callback_call(ToxAV *av, toxav_call_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->ccb = callback;
    av->ccb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

int toxav_friend_exists(const Tox *tox, int32_t friendnumber)
{
    if (tox) {
        bool res = tox_friend_exists(tox, friendnumber);

        if (res) {
            return 1;
        } else {
            return 0;
        }
    }

    return 0;
}

void toxav_callback_call_comm(ToxAV *av, toxav_call_comm_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->call_comm_cb = callback;
    av->call_comm_cb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

bool toxav_answer(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate, uint32_t video_bit_rate,
                  Toxav_Err_Answer *error)
{
    pthread_mutex_lock(av->mutex);

    Toxav_Err_Answer rc = TOXAV_ERR_ANSWER_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if ((audio_bit_rate && audio_bit_rate_invalid(audio_bit_rate))
            || (video_bit_rate && video_bit_rate_invalid(video_bit_rate))
       ) {
        rc = TOXAV_ERR_ANSWER_INVALID_BIT_RATE;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == nullptr) {
        rc = TOXAV_ERR_ANSWER_FRIEND_NOT_CALLING;
        goto RETURN;
    }

    if (!call_prepare_transmission(call)) {
        rc = TOXAV_ERR_ANSWER_CODEC_INITIALIZATION;
        goto RETURN;
    }

    call->audio_bit_rate = audio_bit_rate;
    call->video_bit_rate = video_bit_rate;
    call->video_bit_rate_not_yet_set = call->video_bit_rate;

    LOGGER_ERROR(av->m->log, "toxav_answer:vb=%d", (int)video_bit_rate);

    call->previous_self_capabilities = MSI_CAP_R_AUDIO | MSI_CAP_R_VIDEO;

    call->previous_self_capabilities |= audio_bit_rate > 0 ? MSI_CAP_S_AUDIO : 0;
    call->previous_self_capabilities |= video_bit_rate > 0 ? MSI_CAP_S_VIDEO : 0;

    if (msi_answer(call->msi_call, call->previous_self_capabilities) != 0) {
        rc = TOXAV_ERR_ANSWER_SYNC;
    }

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_ANSWER_OK;
}

void toxav_callback_call_state(ToxAV *av, toxav_call_state_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->scb = callback;
    av->scb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

bool toxav_call_control(ToxAV *av, uint32_t friend_number, Toxav_Call_Control control, Toxav_Err_Call_Control *error)
{
    pthread_mutex_lock(av->mutex);
    Toxav_Err_Call_Control rc = TOXAV_ERR_CALL_CONTROL_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || (!call->active && control != TOXAV_CALL_CONTROL_CANCEL)) {
        rc = TOXAV_ERR_CALL_CONTROL_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    switch (control) {
        case TOXAV_CALL_CONTROL_RESUME: {
            /* Only act if paused and had media transfer active before */
            if (call->msi_call->self_capabilities == 0 &&
                    call->previous_self_capabilities) {

                if (msi_change_capabilities(call->msi_call,
                                            call->previous_self_capabilities) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving_mark(av->tox, call->audio_rtp);
                rtp_allow_receiving_mark(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_PAUSE: {
            /* Only act if not already paused */
            if (call->msi_call->self_capabilities) {
                call->previous_self_capabilities = call->msi_call->self_capabilities;

                if (msi_change_capabilities(call->msi_call, 0) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving_mark(av->tox, call->audio_rtp);
                rtp_stop_receiving_mark(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_CANCEL: {
            /* Hang up */
            pthread_mutex_lock(call->toxav_call_mutex);

            if (msi_hangup(call->msi_call) != 0) {
                rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                pthread_mutex_unlock(call->toxav_call_mutex);
                goto RETURN;
            }

            call->msi_call = nullptr;
            pthread_mutex_unlock(call->toxav_call_mutex);

            /* No mather the case, terminate the call */
            call_kill_transmission(call);
            call_remove(call);
        }
        break;

        case TOXAV_CALL_CONTROL_MUTE_AUDIO: {
            if (call->msi_call->self_capabilities & MSI_CAP_R_AUDIO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ MSI_CAP_R_AUDIO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving_mark(av->tox, call->audio_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_UNMUTE_AUDIO: {
            if (call->msi_call->self_capabilities ^ MSI_CAP_R_AUDIO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_R_AUDIO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving_mark(av->tox, call->audio_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_HIDE_VIDEO: {
            if (call->msi_call->self_capabilities & MSI_CAP_R_VIDEO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities ^ MSI_CAP_R_VIDEO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_stop_receiving_mark(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;

        case TOXAV_CALL_CONTROL_SHOW_VIDEO: {
            if (call->msi_call->self_capabilities ^ MSI_CAP_R_VIDEO) {
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_R_VIDEO) == -1) {
                    rc = TOXAV_ERR_CALL_CONTROL_SYNC;
                    goto RETURN;
                }

                rtp_allow_receiving_mark(av->tox, call->video_rtp);
            } else {
                rc = TOXAV_ERR_CALL_CONTROL_INVALID_TRANSITION;
                goto RETURN;
            }
        }
        break;
    }

RETURN:
    pthread_mutex_unlock(av->mutex);

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_CALL_CONTROL_OK;
}

bool toxav_option_set(ToxAV *av, uint32_t friend_number, TOXAV_OPTIONS_OPTION option, int32_t value,
                      TOXAV_ERR_OPTION_SET *error)
{
    TOXAV_ERR_OPTION_SET rc = TOXAV_ERR_OPTION_SET_OK;

    ToxAVCall *call;

    LOGGER_DEBUG(av->m->log, "toxav_option_set:1 %d %d", (int)option, (int)value);

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_OPTION_SET_OTHER_ERROR;
        goto END;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_OPTION_SET_OTHER_ERROR;
        goto END;
    }

    pthread_mutex_lock(call->toxav_call_mutex);

    LOGGER_DEBUG(av->m->log, "toxav_option_set:2 %d %d", (int)option, (int)value);

    if (option == TOXAV_ENCODER_CPU_USED) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_encoder_cpu_used == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder cpu_used already set to: %d", (int)value);
        } else {
            vc->video_encoder_cpu_used_prev = vc->video_encoder_cpu_used;
            vc->video_encoder_cpu_used = (int32_t)value;
            LOGGER_WARNING(av->m->log, "video encoder setting cpu_used to: %d", (int)value);
        }
    } else if (option == TOXAV_CLIENT_VIDEO_CAPTURE_DELAY_MS) {
        VCSession *vc = (VCSession *)call->video;

        if (((int32_t)value >= 0)
                && ((int32_t)value <= 10000)) {
            vc->client_video_capture_delay_ms = (int32_t)value;
        }
    } else if (option == TOXAV_ENCODER_CODEC_USED) {
        VCSession *vc = (VCSession *)call->video;

        if (((int32_t)value >= TOXAV_ENCODER_CODEC_USED_VP8)
                && ((int32_t)value <= TOXAV_ENCODER_CODEC_USED_H264)) {

            if (vc->video_encoder_coded_used == (int32_t)value) {
                LOGGER_WARNING(av->m->log, "video video_encoder_coded_used already set to: %d", (int)value);
            } else {
                vc->video_encoder_coded_used_prev = vc->video_encoder_coded_used;
                vc->video_encoder_coded_used = (int32_t)value;
                LOGGER_WARNING(av->m->log, "video video_encoder_coded_used to: %d", (int)value);
            }
        }
    } else if (option == TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION) {
        VCSession *vc = (VCSession *)call->video;

        if (((int32_t)value >= TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_0)
                && ((int32_t)value <= TOXAV_CLIENT_INPUT_VIDEO_ORIENTATION_270)) {

            if (vc->video_encoder_frame_orientation_angle == (int32_t)value) {
                LOGGER_WARNING(av->m->log, "video video_encoder_frame_orientation_angle already set to: %d", (int)value);
            } else {
                vc->video_encoder_frame_orientation_angle = (int32_t)value;
                LOGGER_WARNING(av->m->log, "video video_encoder_frame_orientation_angle to: %d", (int)value);
            }
        }
    } else if (option == TOXAV_ENCODER_VP8_QUALITY) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_encoder_vp8_quality == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder vp8_quality already set to: %d", (int)value);
        } else {
            vc->video_encoder_vp8_quality_prev = vc->video_encoder_vp8_quality;
            vc->video_encoder_vp8_quality = (int32_t)value;

            if (vc->video_encoder_vp8_quality == TOXAV_ENCODER_VP8_QUALITY_HIGH) {
                vc->video_rc_max_quantizer_prev = vc->video_rc_max_quantizer;
                vc->video_rc_min_quantizer_prev = vc->video_rc_min_quantizer;
                vc->video_rc_max_quantizer = TOXAV_ENCODER_VP8_RC_MAX_QUANTIZER_HIGH;
                vc->video_rc_min_quantizer = TOXAV_ENCODER_VP8_RC_MIN_QUANTIZER_HIGH;
            } else {
                vc->video_rc_max_quantizer_prev = vc->video_rc_max_quantizer;
                vc->video_rc_min_quantizer_prev = vc->video_rc_min_quantizer;
                vc->video_rc_max_quantizer = TOXAV_ENCODER_VP8_RC_MAX_QUANTIZER_NORMAL;
                vc->video_rc_min_quantizer = TOXAV_ENCODER_VP8_RC_MIN_QUANTIZER_NORMAL;
            }

            LOGGER_WARNING(av->m->log, "video encoder setting vp8_quality to: %d", (int)value);
        }
    } else if (option == TOXAV_ENCODER_RC_MAX_QUANTIZER) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_rc_max_quantizer == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder rc_max_quantizer already set to: %d", (int)value);
        } else {
            if ((value >= AV_BUFFERING_MS_MIN) && (value <= AV_BUFFERING_MS_MAX)) {
                vc->video_rc_max_quantizer_prev = vc->video_rc_max_quantizer;
                vc->video_rc_max_quantizer = (int32_t)value;
                LOGGER_WARNING(av->m->log, "video encoder setting rc_max_quantizer to: %d", (int)value);
            }
        }
    } else if (option == TOXAV_DECODER_VIDEO_ADD_DELAY_MS) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_decoder_add_delay_ms == (int32_t)value) {
            LOGGER_DEBUG(av->m->log, "video decoder video_decoder_add_delay_ms already set to: %d", (int)value);
        } else {

            if (((int32_t)value < -200) || ((int32_t)value > 1200)) {
                LOGGER_DEBUG(av->m->log, "video decoder video_decoder_add_delay_ms value outside of valid range: %d", (int)value);
            } else {
                vc->video_decoder_add_delay_ms = (int32_t)value;
            }

            ACSession *ac = (ACSession *)call->audio;
            ac->video_decoder_add_delay_ms_copy = vc->video_decoder_add_delay_ms;
            vc->video_decoder_buffer_sum_ms = vc->video_decoder_buffer_ms + vc->video_decoder_add_delay_ms;
            LOGGER_DEBUG(av->m->log, "video decoder setting video_decoder_add_delay_ms to: %d",
                         (int)vc->video_decoder_buffer_sum_ms);
        }
    } else if (option == TOXAV_DECODER_VIDEO_BUFFER_MS) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_decoder_buffer_ms == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video decoder video_decoder_buffer_ms already set to: %d", (int)value);
        } else {
            vc->video_decoder_buffer_ms = (int32_t)value;

            if ((vc->video_decoder_buffer_ms - AV_BUFFERING_DELTA_MS) > 1) {
                vc->video_decoder_adjustment_base_ms = vc->video_decoder_buffer_ms - AV_BUFFERING_DELTA_MS;
            } else {
                vc->video_decoder_adjustment_base_ms = 1;
            }

            vc->video_decoder_buffer_sum_ms = vc->video_decoder_buffer_ms + vc->video_decoder_add_delay_ms;

            LOGGER_WARNING(av->m->log, "video decoder setting video_decoder_buffer_ms to: %d", (int)value);
        }
    } else if (option == TOXAV_ENCODER_VIDEO_MAX_BITRATE) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_max_bitrate == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder video_max_bitrate already set to: %d", (int)value);
        } else {
            vc->video_max_bitrate = (int32_t)value;

            if (call->video_bit_rate > (uint32_t)vc->video_max_bitrate) {
                call->video_bit_rate = (uint32_t)vc->video_max_bitrate;
                call->video_bit_rate_not_yet_set = call->video_bit_rate;
            }

            LOGGER_WARNING(av->m->log, "video encoder setting video_max_bitrate to: %d", (int)value);
        }
    } else if (option == TOXAV_ENCODER_VIDEO_BITRATE_AUTOSET) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_bitrate_autoset == (uint8_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder video_bitrate_autoset already set to: %d", (int)value);
        } else {
            vc->video_bitrate_autoset = (uint8_t)value;
            LOGGER_WARNING(av->m->log, "video encoder setting video_bitrate_autoset to: %d", (int)value);
        }
    } else if (option == TOXAV_ENCODER_KF_METHOD) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_keyframe_method == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder keyframe_method already set to: %d", (int)value);
        } else {
            vc->video_keyframe_method_prev = vc->video_keyframe_method;
            vc->video_keyframe_method = (int32_t)value;
            LOGGER_WARNING(av->m->log, "video encoder setting keyframe_method to: %d", (int)value);
        }
    } else if (option == TOXAV_ENCODER_RC_MIN_QUANTIZER) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_rc_min_quantizer == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder video_rc_min_quantizer already set to: %d", (int)value);
        } else {
            vc->video_rc_min_quantizer_prev = vc->video_rc_min_quantizer;
            vc->video_rc_min_quantizer = (int32_t)value;
            LOGGER_WARNING(av->m->log, "video encoder setting video_rc_min_quantizer to: %d", (int)value);
        }
    } else if (option == TOXAV_DECODER_ERROR_CONCEALMENT) {
        VCSession *vc = (VCSession *)call->video;

        if (vc->video_decoder_error_concealment == (int32_t)value) {
            LOGGER_WARNING(av->m->log, "video encoder video_decoder_error_concealment already set to: %d", (int)value);
        } else {
            vc->video_decoder_error_concealment_prev = vc->video_decoder_error_concealment;
            vc->video_decoder_error_concealment = (int32_t)value;
            LOGGER_WARNING(av->m->log, "video encoder setting video_decoder_error_concealment to: %d", (int)value);
        }
    }

    pthread_mutex_unlock(call->toxav_call_mutex);
    pthread_mutex_unlock(av->mutex);
END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_OPTION_SET_OK;
}

bool toxav_video_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t video_bit_rate,
                              Toxav_Err_Bit_Rate_Set *error)
{
    Toxav_Err_Bit_Rate_Set rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (video_bit_rate > 0 && video_bit_rate_invalid(video_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto RETURN;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    LOGGER_DEBUG(av->m->log, "Setting new video bitrate to: %d", video_bit_rate);

    if (call->video_bit_rate == video_bit_rate) {
        LOGGER_DEBUG(av->m->log, "Video bitrate already set to: %d", video_bit_rate);
    } else if (video_bit_rate == 0) {
        LOGGER_DEBUG(av->m->log, "Turned off video sending");

        /* Video sending is turned off; notify peer */
        if (msi_change_capabilities(call->msi_call, call->msi_call->
                                    self_capabilities ^ MSI_CAP_S_VIDEO) != 0) {
            pthread_mutex_unlock(av->mutex);
            rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
            goto RETURN;
        }

        call->video_bit_rate = 0;
        call->video_bit_rate_not_yet_set = call->video_bit_rate;
    } else {
        pthread_mutex_lock(call->toxav_call_mutex);

        if (call->video_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned on video sending");

            /* The video has been turned off before this */
            if (msi_change_capabilities(call->msi_call, call->
                                        msi_call->self_capabilities | MSI_CAP_S_VIDEO) != 0) {
                pthread_mutex_unlock(call->toxav_call_mutex);
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto RETURN;
            }
        } else {
            LOGGER_DEBUG(av->m->log, "Set new video bit rate %d", video_bit_rate);
        }

        call->video_bit_rate = video_bit_rate;
        call->video_bit_rate_not_yet_set = call->video_bit_rate;
        pthread_mutex_unlock(call->toxav_call_mutex);
    }

    pthread_mutex_unlock(av->mutex);
RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}

bool toxav_audio_set_bit_rate(ToxAV *av, uint32_t friend_number, uint32_t audio_bit_rate,
                              Toxav_Err_Bit_Rate_Set *error)
{
    Toxav_Err_Bit_Rate_Set rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (audio_bit_rate > 0 && audio_bit_rate_invalid(audio_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto RETURN;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    LOGGER_DEBUG(av->m->log, "Setting new audio bitrate to: %d", audio_bit_rate);

    if (call->audio_bit_rate == audio_bit_rate) {
        LOGGER_DEBUG(av->m->log, "Audio bitrate already set to: %d", audio_bit_rate);
    } else if (audio_bit_rate == 0) {
        LOGGER_DEBUG(av->m->log, "Turned off audio sending");

        if (msi_change_capabilities(call->msi_call, call->msi_call->
                                    self_capabilities ^ MSI_CAP_S_AUDIO) != 0) {
            pthread_mutex_unlock(av->mutex);
            rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
            goto RETURN;
        }

        /* Audio sending is turned off; notify peer */
        call->audio_bit_rate = 0;
    } else {
        pthread_mutex_lock(call->toxav_call_mutex);

        if (call->audio_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned on audio sending");

            /* The audio has been turned off before this */
            if (msi_change_capabilities(call->msi_call, call->
                                        msi_call->self_capabilities | MSI_CAP_S_AUDIO) != 0) {
                pthread_mutex_unlock(call->toxav_call_mutex);
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto RETURN;
            }
        } else {
            LOGGER_DEBUG(av->m->log, "Set new audio bit rate %d", audio_bit_rate);
        }

        call->audio_bit_rate = audio_bit_rate;
        pthread_mutex_unlock(call->toxav_call_mutex);
    }

    pthread_mutex_unlock(av->mutex);
RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}

bool toxav_bit_rate_set(ToxAV *av, uint32_t friend_number, int32_t audio_bit_rate,
                        int32_t video_bit_rate, TOXAV_ERR_BIT_RATE_SET *error)
{
    TOXAV_ERR_BIT_RATE_SET rc = TOXAV_ERR_BIT_RATE_SET_OK;
    ToxAVCall *call;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_FOUND;
        goto END;
    }

    if (audio_bit_rate > 0 && audio_bit_rate_invalid(audio_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto END;
    }

    if (video_bit_rate > 0 && video_bit_rate_invalid(video_bit_rate)) {
        rc = TOXAV_ERR_BIT_RATE_SET_INVALID_BIT_RATE;
        goto END;
    }

    pthread_mutex_lock(av->mutex);
    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_BIT_RATE_SET_FRIEND_NOT_IN_CALL;
        goto END;
    }

    if (audio_bit_rate >= 0) {
        LOGGER_DEBUG(av->m->log, "Setting new audio bitrate to: %d", audio_bit_rate);

        if (call->audio_bit_rate == (uint32_t)audio_bit_rate) {
            LOGGER_DEBUG(av->m->log, "Audio bitrate already set to: %d", audio_bit_rate);
        } else if (audio_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned off audio sending");

            if (msi_change_capabilities(call->msi_call, call->msi_call->
                                        self_capabilities ^ MSI_CAP_S_AUDIO) != 0) {
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto END;
            }

            /* Audio sending is turned off; notify peer */
            call->audio_bit_rate = 0;
        } else {
            pthread_mutex_lock(call->toxav_call_mutex);

            if (call->audio_bit_rate == 0) {
                LOGGER_DEBUG(av->m->log, "Turned on audio sending");

                /* The audio has been turned off before this */
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_S_AUDIO) != 0) {
                    pthread_mutex_unlock(call->toxav_call_mutex);
                    pthread_mutex_unlock(av->mutex);
                    rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                    goto END;
                }
            } else {
                LOGGER_DEBUG(av->m->log, "Set new audio bit rate %d", audio_bit_rate);
            }

            call->audio_bit_rate = audio_bit_rate;
            pthread_mutex_unlock(call->toxav_call_mutex);
        }
    }

    if (video_bit_rate >= 0) {
        LOGGER_DEBUG(av->m->log, "Setting new video bitrate to: %d", video_bit_rate);

        if (call->video_bit_rate == (uint32_t)video_bit_rate) {
            LOGGER_DEBUG(av->m->log, "Video bitrate already set to: %d", video_bit_rate);
        } else if (video_bit_rate == 0) {
            LOGGER_DEBUG(av->m->log, "Turned off video sending");

            /* Video sending is turned off; notify peer */
            if (msi_change_capabilities(call->msi_call, call->msi_call->
                                        self_capabilities ^ MSI_CAP_S_VIDEO) != 0) {
                pthread_mutex_unlock(av->mutex);
                rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                goto END;
            }

            call->video_bit_rate = 0;
            call->video_bit_rate_not_yet_set = call->video_bit_rate;
        } else {
            pthread_mutex_lock(call->toxav_call_mutex);

            if (call->video_bit_rate == 0) {
                LOGGER_DEBUG(av->m->log, "Turned on video sending");

                /* The video has been turned off before this */
                if (msi_change_capabilities(call->msi_call, call->
                                            msi_call->self_capabilities | MSI_CAP_S_VIDEO) != 0) {
                    pthread_mutex_unlock(call->toxav_call_mutex);
                    pthread_mutex_unlock(av->mutex);
                    rc = TOXAV_ERR_BIT_RATE_SET_SYNC;
                    goto END;
                }
            } else {
                LOGGER_DEBUG(av->m->log, "Set new video bit rate %d", video_bit_rate);
            }

            call->video_bit_rate = video_bit_rate;
            call->video_bit_rate_not_yet_set = call->video_bit_rate;

            LOGGER_ERROR(av->m->log, "toxav_bit_rate_set:vb=%d", (int)video_bit_rate);

            pthread_mutex_unlock(call->toxav_call_mutex);
        }
    }

    pthread_mutex_unlock(av->mutex);
END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_BIT_RATE_SET_OK;
}

void toxav_callback_bit_rate_status(ToxAV *av, toxav_bit_rate_status_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->bcb = callback;
    av->bcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}


void toxav_callback_audio_bit_rate(ToxAV *av, toxav_audio_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->abcb = callback;
    av->abcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

void toxav_callback_video_bit_rate(ToxAV *av, toxav_video_bit_rate_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vbcb = callback;
    av->vbcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

bool toxav_audio_send_frame(ToxAV *av, uint32_t friend_number, const int16_t *pcm, size_t sample_count,
                            uint8_t channels, uint32_t sampling_rate, Toxav_Err_Send_Frame *error)
{
    Toxav_Err_Send_Frame rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    uint64_t audio_frame_record_timestamp = current_time_monotonic(av->toxav_mono_time);

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    if (call->audio_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & MSI_CAP_S_AUDIO) ||
            !(call->msi_call->peer_capabilities & MSI_CAP_R_AUDIO)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto RETURN;
    }

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(av->mutex);

    if (pcm == nullptr) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto RETURN;
    }

    if (channels > 2) {
        pthread_mutex_unlock(call->mutex_audio);
        rc = TOXAV_ERR_SEND_FRAME_INVALID;
        goto RETURN;
    }

    { /* Encode and send */
        if (ac_reconfigure_encoder(call->audio, call->audio_bit_rate * 1000, sampling_rate, channels) != 0) {
            pthread_mutex_unlock(call->mutex_audio);
            LOGGER_WARNING(av->m->log, "Failed reconfigure audio encoder");
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto RETURN;
        }

        VLA(uint8_t, dest, sample_count + sizeof(sampling_rate)); /* This is more than enough always */

        sampling_rate = net_htonl(sampling_rate);
        memcpy(dest, &sampling_rate, sizeof(sampling_rate));
        int vrc = opus_encode(call->audio->encoder, pcm, sample_count,
                              dest + sizeof(sampling_rate), SIZEOF_VLA(dest) - sizeof(sampling_rate));

        if (vrc < 0) {
            LOGGER_API_WARNING(av->tox, "Failed to encode frame %s", opus_strerror(vrc));
            pthread_mutex_unlock(call->mutex_audio);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto RETURN;
        }

#if defined(AUDIO_DEBUGGING_SIMULATE_SOME_DATA_LOSS)
        // set last part of audio frame to all zeros
        size_t ten_percent_size = ((size_t)vrc / 10);
        size_t start_offset = ((size_t)vrc - ten_percent_size - 1);
        memset((dest + 4 + start_offset), (int)0, ten_percent_size);
        LOGGER_WARNING(av->m->log, "* audio packet set some ZERO data at the end *");
#endif

#if defined(AUDIO_DEBUGGING_SKIP_FRAMES)
        // skip sending some audio frames
        _debug_count_sent_audio_frames++;

        if (_debug_count_sent_audio_frames > _debug_skip_every_x_audio_frame) {
            call->audio_rtp->sequnum++;
            LOGGER_WARNING(av->m->log, "* audio packet sending SKIPPED * %d", (int)call->audio_rtp->sequnum);
            _debug_count_sent_audio_frames = 0;
        } else {
#endif
            LOGGER_DEBUG(av->m->log, "audio packet record time: seqnum=%d %d", (int)call->audio_rtp->sequnum,
                         (int)audio_frame_record_timestamp);

            uint16_t seq_num_save = call->audio_rtp->sequnum;

            if (rtp_send_data(call->audio_rtp, dest,
                              vrc + sizeof(sampling_rate),
                              false,
                              audio_frame_record_timestamp,
                              VIDEO_FRAGMENT_NUM_NO_FRAG,
                              0,
                              call->audio_bit_rate,
                              0,
                              0,
                              nullptr) != 0) {
                LOGGER_WARNING(av->m->log, "Failed to send audio packet");
                rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
            }


#if defined(AUDIO_DEBUGGING_SKIP_FRAMES)
        }

#endif

    }


    pthread_mutex_unlock(call->mutex_audio);

RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}


/* --- VIDEO EN-CODING happens here --- */
/* --- VIDEO EN-CODING happens here --- */
/* --- VIDEO EN-CODING happens here --- */
bool toxav_video_send_frame(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *y,
                            const uint8_t *u, const uint8_t *v, TOXAV_ERR_SEND_FRAME *error)
{
    return toxav_video_send_frame_age(av, friend_number, width, height, y, u, v, error, 0);
}

bool toxav_video_send_frame_age(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *y,
                                const uint8_t *u, const uint8_t *v, TOXAV_ERR_SEND_FRAME *error, uint32_t age_ms)
{
    TOXAV_ERR_SEND_FRAME rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    // LOGGER_ERROR(av->m->log, "OMX:H:001");

    // add the time the data has already aged (in the client)
    uint64_t video_frame_record_timestamp = current_time_monotonic(av->toxav_mono_time) + age_ms;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto END;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto END;
    }

    call = call_get(av, friend_number);

    if (call == NULL || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto END;
    }

    // LOGGER_ERROR(av->m->log, "VIDEO:Skipping:%d %d",
    //            (int)call->video.second->skip_fps,
    //            (int)call->video.second->skip_fps_counter);

    if (call->video->skip_fps != 0) {
        call->video->skip_fps_counter++;

        if (call->video->skip_fps_duration_until_ts > current_time_monotonic(av->toxav_mono_time)) {
            // HINT: ok stop skipping frames now, and reset the values
            call->video->skip_fps = 0;
            call->video->skip_fps_duration_until_ts = 0;
        } else {

            if (call->video->skip_fps_counter == call->video->skip_fps) {
                LOGGER_DEBUG(av->m->log, "VIDEO:Skipping frame, because of too much FPS!!");
                call->video->skip_fps_counter = 0;
                // skip this video frame, receiver can't handle this many FPS
                // rc = TOXAV_ERR_SEND_FRAME_INVALID; // should we tell the client? not sure about this
                //                                     // client may try to resend the frame, which is not what we want
                pthread_mutex_unlock(av->mutex);
                goto END;
            }
        }
    }

    uint64_t ms_to_last_frame = 1;

    if (call->video) {
        ms_to_last_frame = current_time_monotonic(av->toxav_mono_time) - call->video->last_encoded_frame_ts;

        if (call->video->last_encoded_frame_ts == 0) {
            ms_to_last_frame = 1;
        }
    }

    if (call->video_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & MSI_CAP_S_VIDEO) ||
            !(call->msi_call->peer_capabilities & MSI_CAP_R_VIDEO)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto END;
    }

    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(av->mutex);

    if (y == NULL || u == NULL || v == NULL) {
        pthread_mutex_unlock(call->mutex_video);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto END;
    }


    // LOGGER_ERROR(av->m->log, "h264_video_capabilities_received=%d",
    //             (int)call->video->h264_video_capabilities_received);

    int16_t force_reinit_encoder = -1;

    // HINT: auto switch encoder, if we got capabilities packet from friend ------
    if ((call->video->h264_video_capabilities_received == 1)
            &&
            (call->video->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_H264)) {
        // when switching to H264 set default video bitrate

        if (call->video_bit_rate > 0) {
            call->video_bit_rate = VIDEO_BITRATE_INITIAL_VALUE_H264;
            call->video_bit_rate_not_yet_set = call->video_bit_rate;
        }

        call->video->video_encoder_coded_used = TOXAV_ENCODER_CODEC_USED_H264;
        // LOGGER_ERROR(av->m->log, "TOXAV_ENCODER_CODEC_USED_H264");
        force_reinit_encoder = -2;

        if (av->call_comm_cb) {

            TOXAV_CALL_COMM_INFO cmi;
            cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264;

            if (call->video->video_encoder_coded_used_hw_accel == TOXAV_ENCODER_CODEC_HW_ACCEL_OMX_PI) {
                cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264_OMX_PI;
            }

            av->call_comm_cb(av, friend_number, cmi,
                             0, av->call_comm_cb_user_data);
        }

        // reset flag again
        call->video->h264_video_capabilities_received = 0;

    }

    // HINT: auto switch encoder, if we got capabilities packet from friend ------

    if ((call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
            || (call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {

        if (vc_reconfigure_encoder(nullptr, call->video, call->video_bit_rate * 1000,
                                   width, height, -1) != 0) {
            pthread_mutex_unlock(call->mutex_video);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }
    } else {
        // HINT: H264
        if (vc_reconfigure_encoder(nullptr, call->video, call->video_bit_rate * 1000,
                                   width, height, force_reinit_encoder) != 0) {
            pthread_mutex_unlock(call->mutex_video);
            rc = TOXAV_ERR_SEND_FRAME_INVALID;
            goto END;
        }
    }

    if ((call->video_bit_rate_last_last_changed_cb_ts + 500) < current_time_monotonic(av->toxav_mono_time)) {
        if (call->video_bit_rate_last_last_changed != call->video_bit_rate) {
            if (av->call_comm_cb) {
                av->call_comm_cb(av, friend_number,
                                 TOXAV_CALL_COMM_ENCODER_CURRENT_BITRATE,
                                 (int64_t)call->video_bit_rate,
                                 av->call_comm_cb_user_data);
            }

            call->video_bit_rate_last_last_changed = call->video_bit_rate;
        }

        call->video_bit_rate_last_last_changed_cb_ts = current_time_monotonic(av->toxav_mono_time);
    }

    int vpx_encode_flags = 0;
    unsigned long max_encode_time_in_us = MAX_ENCODE_TIME_US;

    int h264_iframe_factor = 1;

    if (call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_H264) {
        h264_iframe_factor = 1;
    }

    if (call->video->video_keyframe_method == TOXAV_ENCODER_KF_METHOD_NORMAL) {
        if (call->video_rtp->ssrc < (uint32_t)(VIDEO_SEND_X_KEYFRAMES_FIRST * h264_iframe_factor)) {

            if (call->video->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_VP9) {
                // Key frame flag for first frames
                vpx_encode_flags = VPX_EFLAG_FORCE_KF;
                vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
                // vpx_encode_flags |= VP8_EFLAG_FORCE_ARF;

                max_encode_time_in_us = VPX_DL_REALTIME;
                // uint32_t lowered_bitrate = (300 * 1000);
                // vc_reconfigure_encoder_bitrate_only(call->video, lowered_bitrate);
                // HINT: Zoff: this does not seem to work
                // vpx_codec_control(call->video->encoder, VP8E_SET_FRAME_FLAGS, vpx_encode_flags);
                // LOGGER_ERROR(av->m->log, "I_FRAME_FLAG:%d only-i-frame mode", call->video_rtp->ssrc);
            }

#ifdef RASPBERRY_PI_OMX
            LOGGER_ERROR(av->m->log, "H264: force i-frame");
            h264_omx_raspi_force_i_frame(av->m->log, call->video);
#endif

            call->video_rtp->ssrc++;
        } else if (call->video_rtp->ssrc == (uint32_t)(VIDEO_SEND_X_KEYFRAMES_FIRST * h264_iframe_factor)) {
            if (call->video->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_VP9) {
                // normal keyframe placement
                vpx_encode_flags = 0;
                max_encode_time_in_us = MAX_ENCODE_TIME_US;
                LOGGER_INFO(av->m->log, "I_FRAME_FLAG:%d normal mode", call->video_rtp->ssrc);
            }

            call->video_rtp->ssrc++;
        }
    }


#ifdef VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE
    long encode_time_auto_tune = MAX_ENCODE_TIME_US;

    if (call->video->last_encoded_frame_ts > 0) {
        encode_time_auto_tune = (current_time_monotonic(av->toxav_mono_time) - call->video->last_encoded_frame_ts) * 1000;
#ifdef VIDEO_CODEC_ENCODER_USE_FRAGMENTS
        encode_time_auto_tune = encode_time_auto_tune * VIDEO_CODEC_FRAGMENT_NUMS;
#endif

        if (encode_time_auto_tune == 0) {
            // if the real delay was 0ms then still use 1ms
            encode_time_auto_tune = 1;
        }

        if (call->video->encoder_soft_deadline[call->video->encoder_soft_deadline_index] == 0) {
            call->video->encoder_soft_deadline[call->video->encoder_soft_deadline_index] = 1;
            LOGGER_DEBUG(av->m->log, "AUTOTUNE: delay=[1]");
        } else {
            call->video->encoder_soft_deadline[call->video->encoder_soft_deadline_index] = encode_time_auto_tune;
            LOGGER_DEBUG(av->m->log, "AUTOTUNE: delay=%d", (int)encode_time_auto_tune);
        }

        call->video->encoder_soft_deadline_index = (call->video->encoder_soft_deadline_index + 1) %
                VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES;

        // calc mean value
        encode_time_auto_tune = 0;

        for (int k = 0; k < VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES; k++) {
            encode_time_auto_tune = encode_time_auto_tune + call->video->encoder_soft_deadline[k];
        }

        encode_time_auto_tune = encode_time_auto_tune / VIDEO_ENCODER_SOFT_DEADLINE_AUTOTUNE_ENTRIES;

        if (encode_time_auto_tune > (1000000 / VIDEO_ENCODER_MINFPS_AUTOTUNE)) {
            encode_time_auto_tune = (1000000 / VIDEO_ENCODER_MINFPS_AUTOTUNE);
        }

        if (encode_time_auto_tune > (VIDEO_ENCODER_LEEWAY_IN_MS_AUTOTUNE * 1000)) {
            encode_time_auto_tune = encode_time_auto_tune - (VIDEO_ENCODER_LEEWAY_IN_MS_AUTOTUNE * 1000); // give x ms more room
        }

        if (encode_time_auto_tune == 0) {
            // if the real delay was 0ms then still use 1ms
            encode_time_auto_tune = 1;
        }
    }

    max_encode_time_in_us = encode_time_auto_tune;
    LOGGER_DEBUG(av->m->log, "AUTOTUNE:MAX_ENCODE_TIME_US=%ld us = %.1f fps", (long)encode_time_auto_tune,
                 (float)(1000000.0f / encode_time_auto_tune));
#endif


    // we start with I-frames (full frames) and then switch to normal mode later

    call->video->last_encoded_frame_ts = current_time_monotonic(av->toxav_mono_time);

    if (call->video->send_keyframe_request_received == 1) {
        vpx_encode_flags = VPX_EFLAG_FORCE_KF;
        vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
        // vpx_encode_flags |= VP8_EFLAG_FORCE_ARF;
        call->video->send_keyframe_request_received = 0;
    } else {
        LOGGER_DEBUG(av->m->log, "++++ FORCE KEYFRAME ++++:%d %d %d",
                     (int)call->video->last_sent_keyframe_ts,
                     (int)VIDEO_MIN_SEND_KEYFRAME_INTERVAL,
                     (int)current_time_monotonic(av->toxav_mono_time));

        if ((call->video->last_sent_keyframe_ts + VIDEO_MIN_SEND_KEYFRAME_INTERVAL)
                < current_time_monotonic(av->toxav_mono_time)) {
            // it's been x seconds without a keyframe, send one now
            vpx_encode_flags = VPX_EFLAG_FORCE_KF;
            vpx_encode_flags |= VP8_EFLAG_FORCE_GF;

            LOGGER_DEBUG(av->m->log, "1***** FORCE KEYFRAME ++++");
            LOGGER_DEBUG(av->m->log, "2***** FORCE KEYFRAME ++++");
            LOGGER_DEBUG(av->m->log, "3***** FORCE KEYFRAME ++++");
            LOGGER_DEBUG(av->m->log, "4***** FORCE KEYFRAME ++++");
            // vpx_encode_flags |= VP8_EFLAG_FORCE_ARF;
        } else {
            // vpx_encode_flags |= VP8_EFLAG_FORCE_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_ARF;
            // vpx_encode_flags |= VP8_EFLAG_NO_REF_LAST;
            // vpx_encode_flags |= VP8_EFLAG_NO_UPD_GF;
            // vpx_encode_flags |= VP8_EFLAG_NO_UPD_ARF;
        }
    }


    // for the H264 encoder -------
    x264_nal_t *nal = NULL;
    int i_frame_size = 0;
    // for the H264 encoder -------


    { /* Encode */


        if ((call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
                || (call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {

            LOGGER_ERROR(av->m->log, "++++++ encoding VP8 frame ++++++");

            // HINT: vp8
            uint32_t result = encode_frame_vpx(av, friend_number, width, height,
                                               y, u, v, call,
                                               &video_frame_record_timestamp,
                                               vpx_encode_flags,
                                               &nal,
                                               &i_frame_size);

            if (result != 0) {
                pthread_mutex_unlock(call->mutex_video);
                rc = TOXAV_ERR_SEND_FRAME_INVALID;
                goto END;
            }

        } else {

            LOGGER_DEBUG(av->m->log, "**##** encoding H264 frame **##**");

            // HINT: H264
#ifdef RASPBERRY_PI_OMX
            uint32_t result = encode_frame_h264_omx_raspi(av, friend_number, width, height,
                              y, u, v, call,
                              &video_frame_record_timestamp,
                              vpx_encode_flags,
                              &nal,
                              &i_frame_size);
#else
            uint32_t result = encode_frame_h264(av, friend_number, width, height,
                                                y, u, v, call,
                                                &video_frame_record_timestamp,
                                                vpx_encode_flags,
                                                &nal,
                                                &i_frame_size);
#endif

            if (result != 0) {
                pthread_mutex_unlock(call->mutex_video);
                rc = TOXAV_ERR_SEND_FRAME_INVALID;
                goto END;
            }

        }
    }

    ++call->video->frame_counter;

    LOGGER_DEBUG(av->m->log, "VPXENC:======================\n");
    LOGGER_DEBUG(av->m->log, "VPXENC:frame num=%ld\n", (long)call->video->frame_counter);


    { /* Send frames */

        if ((call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP8)
                || (call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_VP9)) {

            uint32_t result = send_frames_vpx(av, friend_number, width, height,
                                              y, u, v, call,
                                              &video_frame_record_timestamp,
                                              vpx_encode_flags,
                                              &nal,
                                              &i_frame_size,
                                              &rc);

            if (result != 0) {
                pthread_mutex_unlock(call->mutex_video);
                goto END;
            }

        } else {
            // HINT: H264
#ifdef RASPBERRY_PI_OMX
            uint32_t result = send_frames_h264_omx_raspi(av, friend_number, width, height,
                              y, u, v, call,
                              &video_frame_record_timestamp,
                              vpx_encode_flags,
                              &nal,
                              &i_frame_size,
                              &rc);

#else
            uint32_t result = send_frames_h264(av, friend_number, width, height,
                                               y, u, v, call,
                                               &video_frame_record_timestamp,
                                               vpx_encode_flags,
                                               &nal,
                                               &i_frame_size,
                                               &rc);
#endif

            if (result != 0) {
                pthread_mutex_unlock(call->mutex_video);
                goto END;
            }
        }
    }

    pthread_mutex_unlock(call->mutex_video);

END:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;
}
/* --- VIDEO EN-CODING happens here --- */
/* --- VIDEO EN-CODING happens here --- */
/* --- VIDEO EN-CODING happens here --- */

bool toxav_video_send_frame_h264(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height, const uint8_t *buf,
                                 uint32_t data_len, TOXAV_ERR_SEND_FRAME *error)
{
    return toxav_video_send_frame_h264_age(av, friend_number, width, height, buf, data_len, error, 0);
}

bool toxav_video_send_frame_h264_age(ToxAV *av, uint32_t friend_number, uint16_t width, uint16_t height,
                                     const uint8_t *buf,
                                     uint32_t data_len, TOXAV_ERR_SEND_FRAME *error, uint32_t age_ms)
{
    TOXAV_ERR_SEND_FRAME rc = TOXAV_ERR_SEND_FRAME_OK;
    ToxAVCall *call;

    // add the time the data has already aged (in the client)
    uint64_t video_frame_record_timestamp = current_time_monotonic(av->toxav_mono_time) + age_ms;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    if (pthread_mutex_trylock(av->mutex) != 0) {
        rc = TOXAV_ERR_SEND_FRAME_SYNC;
        goto RETURN;
    }

    call = call_get(av, friend_number);

    if (call == nullptr || !call->active || call->msi_call->state != MSI_CALL_ACTIVE) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_FRIEND_NOT_IN_CALL;
        goto RETURN;
    }

    uint64_t ms_to_last_frame = 1;

    if (call->video) {
        ms_to_last_frame = current_time_monotonic(av->toxav_mono_time) - call->video->last_encoded_frame_ts;

        if (call->video->last_encoded_frame_ts == 0) {
            ms_to_last_frame = 1;
        }
    }

    if (call->video_bit_rate == 0 ||
            !(call->msi_call->self_capabilities & MSI_CAP_S_VIDEO) ||
            !(call->msi_call->peer_capabilities & MSI_CAP_R_VIDEO)) {
        pthread_mutex_unlock(av->mutex);
        rc = TOXAV_ERR_SEND_FRAME_PAYLOAD_TYPE_DISABLED;
        goto RETURN;
    }

    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(av->mutex);

    if (buf == NULL) {
        pthread_mutex_unlock(call->mutex_video);
        rc = TOXAV_ERR_SEND_FRAME_NULL;
        goto RETURN;
    }

    int16_t force_reinit_encoder = -1;

    // HINT: auto switch encoder, if we got capabilities packet from friend ------
    if ((call->video->h264_video_capabilities_received == 1)
            &&
            (call->video->video_encoder_coded_used != TOXAV_ENCODER_CODEC_USED_H264)) {
        // when switching to H264 set default video bitrate

        if (call->video_bit_rate > 0) {
            call->video_bit_rate = VIDEO_BITRATE_INITIAL_VALUE_H264;
            call->video_bit_rate_not_yet_set = call->video_bit_rate;
        }

        call->video->video_encoder_coded_used = TOXAV_ENCODER_CODEC_USED_H264;
        // LOGGER_ERROR(av->m->log, "TOXAV_ENCODER_CODEC_USED_H264");
        force_reinit_encoder = -2;

        if (av->call_comm_cb) {

            TOXAV_CALL_COMM_INFO cmi;
            cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264;

            if (call->video->video_encoder_coded_used_hw_accel == TOXAV_ENCODER_CODEC_HW_ACCEL_OMX_PI) {
                cmi = TOXAV_CALL_COMM_ENCODER_IN_USE_H264_OMX_PI;
            }

            av->call_comm_cb(av, friend_number, cmi,
                             0, av->call_comm_cb_user_data);
        }

        // reset flag again
        call->video->h264_video_capabilities_received = 0;
    }

    // HINT: auto switch encoder, if we got capabilities packet from friend ------


    if ((call->video_bit_rate_last_last_changed_cb_ts + 500) < current_time_monotonic(av->toxav_mono_time)) {
        if (call->video_bit_rate_last_last_changed != call->video_bit_rate) {
            if (av->call_comm_cb) {
                av->call_comm_cb(av, friend_number,
                                 TOXAV_CALL_COMM_ENCODER_CURRENT_BITRATE,
                                 (int64_t)call->video_bit_rate,
                                 av->call_comm_cb_user_data);
            }

            call->video_bit_rate_last_last_changed = call->video_bit_rate;
        }

        call->video_bit_rate_last_last_changed_cb_ts = current_time_monotonic(av->toxav_mono_time);
    }

    call->video->last_encoded_frame_ts = current_time_monotonic(av->toxav_mono_time);

    ++call->video->frame_counter;

    LOGGER_DEBUG(av->m->log, "VPXENC:======================\n");
    LOGGER_DEBUG(av->m->log, "VPXENC:frame num=%ld\n", (long)call->video->frame_counter);


    { /* Send frames */

        // HINT: H264
        uint32_t result = 0;
        const uint32_t frame_length_in_bytes = data_len;
        const int keyframe = (int)0; // TODO: use the actual value!

        LOGGER_DEBUG(av->m->log, "video packet record time: %d", (int)(video_frame_record_timestamp));

        int res = rtp_send_data
                  (
                      call->video_rtp,
                      (const uint8_t *)buf,
                      frame_length_in_bytes,
                      keyframe,
                      video_frame_record_timestamp,
                      (int32_t)0,
                      TOXAV_ENCODER_CODEC_USED_H264,
                      call->video_bit_rate,
                      call->video->client_video_capture_delay_ms,
                      call->video->video_encoder_frame_orientation_angle,
                      nullptr
                  );

        video_frame_record_timestamp++;

        if (res < 0) {
            LOGGER_WARNING(av->m->log, "Could not send video frame: %s", strerror(errno));
            rc = TOXAV_ERR_SEND_FRAME_RTP_FAILED;
            result = 1;
        } else {
            result = 0;
        }


        if (result != 0) {
            pthread_mutex_unlock(call->mutex_video);
            goto RETURN;
        }
    }

    pthread_mutex_unlock(call->mutex_video);

RETURN:

    if (error) {
        *error = rc;
    }

    return rc == TOXAV_ERR_SEND_FRAME_OK;

}


void toxav_callback_audio_receive_frame(ToxAV *av, toxav_audio_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->acb = callback;
    av->acb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

void toxav_callback_video_receive_frame(ToxAV *av, toxav_video_receive_frame_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vcb = callback;
    av->vcb_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}

void toxav_callback_video_receive_frame_h264(ToxAV *av, toxav_video_receive_frame_h264_cb *callback, void *user_data)
{
    pthread_mutex_lock(av->mutex);
    av->vcb_h264 = callback;
    av->vcb_h264_user_data = user_data;
    pthread_mutex_unlock(av->mutex);
}


/*******************************************************************************
 *
 * :: Internal
 *
 ******************************************************************************/
void callback_bwc(BWController *bwc, uint32_t friend_number, float loss, void *user_data)
{
    if (!user_data)
    {
        return;
    }

    ToxAVCall *call = (ToxAVCall *)user_data;

    if (!call) {
        return;
    }

    if (!call->toxav_call_mutex) {
        return;
    }

    if (call->active == 0) {
        return;
    }

    if (!call->av) {
        return;
    }

    if (call->video_bit_rate == 0) {
        // HINT: video is turned off -> just do nothing
        return;
    }

    if (!call->video) {
        return;
    }

    if (call->video->video_bitrate_autoset == 0) {
        // HINT: client does not want bitrate autoset
        return;
    }

    //if (loss > 0) {
    //    LOGGER_ERROR(call->av->m->log, "Reported loss of %f%% : %f", loss * 100, loss);
    //}

    pthread_mutex_lock(call->av->mutex);

    if ((loss * 100) < VIDEO_BITRATE_AUTO_INC_THRESHOLD) {
        if (call->video_bit_rate < VIDEO_BITRATE_MAX_AUTO_VALUE_H264) {

            int64_t tmp = (uint32_t)call->video_bit_rate_not_yet_set;

            if (tmp < VIDEO_BITRATE_SCALAR_AUTO_VALUE_H264) {
                tmp = tmp + VIDEO_BITRATE_SCALAR_INC_BY_AUTO_VALUE_H264;
            } else if (tmp > VIDEO_BITRATE_SCALAR2_AUTO_VALUE_H264) {
                tmp = tmp + VIDEO_BITRATE_SCALAR2_INC_BY_AUTO_VALUE_H264;
            } else {
                tmp = (uint32_t)((float)tmp * (float)VIDEO_BITRATE_AUTO_INC_TO);
            }

            // HINT: sanity check --------------
            if ((uint32_t)tmp < VIDEO_BITRATE_MIN_AUTO_VALUE_H264) {
                tmp = VIDEO_BITRATE_MIN_AUTO_VALUE_H264;
            } else if ((uint32_t)tmp > VIDEO_BITRATE_MAX_AUTO_VALUE_H264) {
                tmp = VIDEO_BITRATE_MAX_AUTO_VALUE_H264;
            }

            if ((uint32_t)tmp > (uint32_t)call->video->video_max_bitrate) {
                tmp = (uint32_t)call->video->video_max_bitrate;
            }

            call->video_bit_rate_not_yet_set = (uint32_t)tmp;

            // HINT: sanity check --------------

            //if ((uint32_t)call->video_bit_rate_not_yet_set > ((uint32_t)call->video_bit_rate + 300)) {
                LOGGER_DEBUG(call->av->m->log, "callback_bwc:INC:vb=%d loss=%d", (int)call->video_bit_rate_not_yet_set,
                             (int)(loss * 100));
                call->video_bit_rate = (uint32_t)call->video_bit_rate_not_yet_set;
            //}
        }
    } else if ((loss * 100) > VIDEO_BITRATE_AUTO_DEC_THRESHOLD) {
        if (call->video_bit_rate > VIDEO_BITRATE_MIN_AUTO_VALUE_H264) {

            int64_t tmp = (int64_t)call->video_bit_rate - (VIDEO_BITRATE_SCALAR_DEC_BY_AUTO_VALUE_H264 * (int)(loss * 100));


            // HINT: sanity check --------------
            if (tmp < VIDEO_BITRATE_MIN_AUTO_VALUE_H264) {
                tmp = VIDEO_BITRATE_MIN_AUTO_VALUE_H264;
            } else if (tmp > VIDEO_BITRATE_MAX_AUTO_VALUE_H264) {
                tmp = VIDEO_BITRATE_MAX_AUTO_VALUE_H264;
            }

            if (tmp > (uint32_t)call->video->video_max_bitrate) {
                tmp = (uint32_t)call->video->video_max_bitrate;
            }

            // HINT: sanity check --------------

#if 0

            if ((loss * 100) > 85) {
                call->video->video_max_bitrate = (uint32_t)((uint32_t)call->video_bit_rate * 0.8f);
                // LOGGER_ERROR(call->av->m->log, "callback_bwc:DEC:2:lower max bitrate to: %d", (int)call->video->video_max_bitrate);
            }

#endif
            //LOGGER_ERROR(call->av->m->log, "callback_bwc:DEC:2:vbold=%d vbnew=%d loss=%d", (int)call->video_bit_rate, (int)tmp,
            //             (int)(loss * 100));

            call->video_bit_rate = (uint32_t)tmp;
            call->video_bit_rate_not_yet_set = call->video_bit_rate;
        }
    }

    // HINT: sanity check --------------

    if (call->video->video_encoder_coded_used == TOXAV_ENCODER_CODEC_USED_H264) {
        if (call->video_bit_rate < VIDEO_BITRATE_MIN_AUTO_VALUE_H264) {
            call->video_bit_rate = VIDEO_BITRATE_MIN_AUTO_VALUE_H264;
        } else if (call->video_bit_rate > VIDEO_BITRATE_MAX_AUTO_VALUE_H264) {
            call->video_bit_rate = VIDEO_BITRATE_MAX_AUTO_VALUE_H264;
        }
    } else {
        if (call->video_bit_rate < VIDEO_BITRATE_MIN_AUTO_VALUE_VP8) {
            call->video_bit_rate = VIDEO_BITRATE_MIN_AUTO_VALUE_VP8;
        } else if (call->video_bit_rate > VIDEO_BITRATE_MAX_AUTO_VALUE_VP8) {
            call->video_bit_rate = VIDEO_BITRATE_MAX_AUTO_VALUE_VP8;
        }

        call->video_bit_rate = (uint32_t)((float)call->video_bit_rate * VIDEO_BITRATE_CORRECTION_FACTOR_VP8);

        if (call->video_bit_rate < VIDEO_BITRATE_MIN_AUTO_VALUE_VP8) {
            call->video_bit_rate = VIDEO_BITRATE_MIN_AUTO_VALUE_VP8;
        }

    }

    if (call->video_bit_rate > (uint32_t)call->video->video_max_bitrate) {
        call->video_bit_rate = (uint32_t)call->video->video_max_bitrate;
    }

    // HINT: sanity check --------------

    pthread_mutex_unlock(call->av->mutex);
}

static int callback_invite(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_new(toxav, call->friend_number, nullptr);

    if (av_call == nullptr) {
        LOGGER_API_WARNING(toxav->tox, "Failed to initialize call...");
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    call->av_call = av_call;
    av_call->msi_call = call;

    if (toxav->ccb) {
        toxav->ccb(toxav, call->friend_number, call->peer_capabilities & MSI_CAP_S_AUDIO,
                   call->peer_capabilities & MSI_CAP_S_VIDEO, toxav->ccb_user_data);
    } else {
        /* No handler to capture the call request, send failure */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

static int callback_start(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    ToxAVCall *av_call = call_get(toxav, call->friend_number);

    if (av_call == nullptr) {
        /* Should this ever happen? */
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!call_prepare_transmission(av_call)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    if (!invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities)) {
        callback_error(toxav_inst, call);
        pthread_mutex_unlock(toxav->mutex);
        return -1;
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

static int callback_end(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_FINISHED);

    if (call->av_call) {
        call_kill_transmission(call->av_call);
        call_remove(call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

static int callback_error(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    invoke_call_state_callback(toxav, call->friend_number, TOXAV_FRIEND_CALL_STATE_ERROR);

    if (call->av_call) {
        call_kill_transmission(call->av_call);
        call_remove(call->av_call);
    }

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

static int callback_capabilites(void *toxav_inst, MSICall *call)
{
    ToxAV *toxav = (ToxAV *)toxav_inst;
    pthread_mutex_lock(toxav->mutex);

    if (call->peer_capabilities & MSI_CAP_S_AUDIO) {
        rtp_allow_receiving_mark(toxav->tox, call->av_call->audio_rtp);
    } else {
        rtp_stop_receiving_mark(toxav->tox, call->av_call->audio_rtp);
    }

    if (call->peer_capabilities & MSI_CAP_S_VIDEO) {
        rtp_allow_receiving_mark(toxav->tox, call->av_call->video_rtp);
    } else {
        rtp_stop_receiving_mark(toxav->tox, call->av_call->video_rtp);
    }

    invoke_call_state_callback(toxav, call->friend_number, call->peer_capabilities);

    pthread_mutex_unlock(toxav->mutex);
    return 0;
}

static bool audio_bit_rate_invalid(uint32_t bit_rate)
{
    /* Opus RFC 6716 section-2.1.1 dictates the following:
     * Opus supports all bit rates from 6 kbit/s to 510 kbit/s.
     */
    return bit_rate < 6 || bit_rate > 510;
}

static bool video_bit_rate_invalid(uint32_t bit_rate)
{
    (void) bit_rate;
    // TODO: remove this, its useless
    return false;
}

static bool invoke_call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state)
{
    if (av->scb) {
        av->scb(av, friend_number, state, av->scb_user_data);
    } else {
        return false;
    }

    return true;
}

static ToxAVCall *call_new(ToxAV *av, uint32_t friend_number, Toxav_Err_Call *error)
{
    /* Assumes mutex locked */
    Toxav_Err_Call rc = TOXAV_ERR_CALL_OK;
    ToxAVCall *call = nullptr;

    if (toxav_friend_exists(av->tox, friend_number) == 0) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_FOUND;
        goto RETURN;
    }

    TOX_ERR_FRIEND_QUERY f_con_query_error;
    TOX_CONNECTION f_conn_status = tox_friend_get_connection_status(av->tox, friend_number, &f_con_query_error);

    if (f_conn_status == TOX_CONNECTION_NONE) {
        rc = TOXAV_ERR_CALL_FRIEND_NOT_CONNECTED;
        goto RETURN;
    }

    if (call_get(av, friend_number) != nullptr) {
        rc = TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL;
        goto RETURN;
    }

    call = (ToxAVCall *)calloc(sizeof(ToxAVCall), 1);

    call->last_incoming_video_frame_rtimestamp = 0;
    call->last_incoming_video_frame_ltimestamp = 0;

    call->last_incoming_audio_frame_rtimestamp = 0;
    call->last_incoming_audio_frame_ltimestamp = 0;

    call->reference_rtimestamp = 0;
    call->reference_ltimestamp = 0;
    call->reference_diff_timestamp = 0;
    call->reference_diff_timestamp_set = 0;

    if (call == nullptr) {
        rc = TOXAV_ERR_CALL_MALLOC;
        goto RETURN;
    }

    call->av = av;
    call->friend_number = friend_number;

    if (create_recursive_mutex(call->toxav_call_mutex)) {
        free(call);
        call = nullptr;
        rc = TOXAV_ERR_CALL_MALLOC;
        goto RETURN;
    }

    if (av->calls == nullptr) { /* Creating */
        av->calls = (ToxAVCall **)calloc(sizeof(ToxAVCall *), friend_number + 1);

        if (av->calls == nullptr) {
            pthread_mutex_destroy(call->toxav_call_mutex);
            free(call);
            call = nullptr;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto RETURN;
        }

        av->calls_tail = friend_number;
        av->calls_head = friend_number;
    } else if (av->calls_tail < friend_number) { /* Appending */
        ToxAVCall **tmp = (ToxAVCall **)realloc(av->calls, sizeof(ToxAVCall *) * (friend_number + 1));

        if (tmp == nullptr) {
            pthread_mutex_destroy(call->toxav_call_mutex);
            free(call);
            call = nullptr;
            rc = TOXAV_ERR_CALL_MALLOC;
            goto RETURN;
        }

        av->calls = tmp;

        /* Set fields in between to null */
        for (uint32_t i = av->calls_tail + 1; i < friend_number; ++i) {
            av->calls[i] = nullptr;
        }

        call->prev = av->calls[av->calls_tail];
        av->calls[av->calls_tail]->next = call;

        av->calls_tail = friend_number;
    } else if (av->calls_head > friend_number) { /* Inserting at front */
        call->next = av->calls[av->calls_head];
        av->calls[av->calls_head]->prev = call;
        av->calls_head = friend_number;
    }

    av->calls[friend_number] = call;

RETURN:

    if (error) {
        *error = rc;
    }

    return call;
}

static ToxAVCall *call_remove(ToxAVCall *call)
{
    if (call == nullptr) {
        return nullptr;
    }

    uint32_t friend_number = call->friend_number;
    ToxAV *av = call->av;

    ToxAVCall *prev = call->prev;
    ToxAVCall *next = call->next;

    /* Set av call in msi to NULL in order to know if call in ToxAVCall is
     * removed from the msi call.
     */
    if (call->msi_call) {
        call->msi_call->av_call = nullptr;
    }

    pthread_mutex_destroy(call->toxav_call_mutex);
    free(call);

    if (prev) {
        prev->next = next;
    } else if (next) {
        av->calls_head = next->friend_number;
    } else {
        goto CLEAR;
    }

    if (next) {
        next->prev = prev;
    } else if (prev) {
        av->calls_tail = prev->friend_number;
    } else {
        goto CLEAR;
    }

    av->calls[friend_number] = nullptr;
    return next;

CLEAR:
    av->calls_head = 0;
    av->calls_tail = 0;
    free(av->calls);
    av->calls = nullptr;

    return nullptr;
}

static bool call_prepare_transmission(ToxAVCall *call)
{
    /* Assumes mutex locked */

    if (call == nullptr) {
        return false;
    }

    ToxAV *av = call->av;

    if (!av->acb && !av->vcb) {
        /* It makes no sense to have CSession without callbacks */
        return false;
    }

    if (call->active) {
        LOGGER_API_WARNING(av->tox, "Call already active!");
        return true;
    }

    if (create_recursive_mutex(call->mutex_audio) != 0) {
        return false;
    }

    if (create_recursive_mutex(call->mutex_video) != 0) {
        goto FAILURE_2;
    }

    /* Prepare bwc */
    call->bwc = bwc_new(av->tox, av->toxav_mono_time, call->friend_number, callback_bwc, call);

    { /* Prepare audio */
        call->audio = ac_new(av->toxav_mono_time, nullptr, av, av->tox, call->friend_number, av->acb, av->acb_user_data);

        if (!call->audio) {
            LOGGER_API_ERROR(av->tox, "Failed to create audio codec session");
            goto FAILURE;
        }

        call->audio_rtp = rtp_new(RTP_TYPE_AUDIO, av->tox, av, call->friend_number, call->bwc,
                                  call->audio, ac_queue_message);

        if (!call->audio_rtp) {
            LOGGER_API_ERROR(av->tox, "Failed to create audio rtp session");
            goto FAILURE;
        }
    }

    { /* Prepare video */
        call->video = vc_new(av->toxav_mono_time, nullptr, av, call->friend_number, av->vcb, av->vcb_user_data);

        if (!call->video) {
            LOGGER_API_ERROR(av->tox, "Failed to create video codec session");
            goto FAILURE;
        }

        call->video_rtp = rtp_new(RTP_TYPE_VIDEO, av->tox, av, call->friend_number, call->bwc,
                                  call->video, vc_queue_message);

        if (!call->video_rtp) {
            LOGGER_API_ERROR(av->tox, "Failed to create video rtp session");
            goto FAILURE;
        }
    }

    call->active = 1;
    return true;

FAILURE:
    bwc_kill(call->bwc);
    rtp_kill(av->tox, call->audio_rtp);
    ac_kill(call->audio);
    call->audio_rtp = nullptr;
    call->audio = nullptr;
    rtp_kill(av->tox, call->video_rtp);
    vc_kill(call->video);
    call->video_rtp = nullptr;
    call->video = nullptr;
    pthread_mutex_destroy(call->mutex_video);
FAILURE_2:
    pthread_mutex_destroy(call->mutex_audio);
    return false;
}

static void call_kill_transmission(ToxAVCall *call)
{
    if (call == nullptr || call->active == 0) {
        return;
    }

    call->active = 0;

    pthread_mutex_lock(call->mutex_audio);
    pthread_mutex_unlock(call->mutex_audio);
    pthread_mutex_lock(call->mutex_video);
    pthread_mutex_unlock(call->mutex_video);

    pthread_mutex_lock(call->toxav_call_mutex);
    bwc_kill(call->bwc);
    pthread_mutex_unlock(call->toxav_call_mutex);

    ToxAV *av = call->av;

    rtp_kill(av->tox, call->audio_rtp);
    ac_kill(call->audio);
    call->audio_rtp = nullptr;
    call->audio = nullptr;

    rtp_kill(av->tox, call->video_rtp);
    vc_kill(call->video);
    call->video_rtp = nullptr;
    call->video = nullptr;

    pthread_mutex_destroy(call->mutex_audio);
    pthread_mutex_destroy(call->mutex_video);
}

Mono_Time *toxav_get_av_mono_time(ToxAV *toxav)
{
    if (!toxav) {
        return nullptr;
    }

    return toxav->toxav_mono_time;
}

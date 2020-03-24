/*
 * Copyright © 2016-2018 The TokTok team.
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

#include "bwcontroller.h"

#include "ring_buffer.h"

#include "../toxcore/logger.h"
#include "../toxcore/mono_time.h"
#include "../toxcore/util.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>


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
/*
 * Zoff: disable logging in ToxAV for now
 */



#define BWC_PACKET_ID (196)
#define BWC_SEND_INTERVAL_MS (200)     // in milliseconds

/**
 *
 */

typedef struct BWCCycle {
    uint32_t last_recv_timestamp; /* Last recv update time stamp */
    uint32_t last_sent_timestamp; /* Last sent update time stamp */
    uint32_t last_refresh_timestamp; /* Last refresh time stamp */

    uint32_t lost;
    uint32_t recv;
} BWCCycle;

struct BWController_s {
    m_cb *mcb;
    void *mcb_user_data;
    Tox *tox;
    uint32_t friend_number;
    BWCCycle cycle;
    Mono_Time* bwc_mono_time;
    uint32_t packet_loss_counted_cycles;
};

struct BWCMessage {
    uint32_t lost;
    uint32_t recv;
};

static int bwc_send_custom_lossy_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length);

void bwc_handle_data(Tox *tox, uint32_t friendnumber, const uint8_t *data, size_t length, void *dummy);
void send_update(BWController *bwc, bool force_update_now);

BWController *bwc_new(Tox *tox, Mono_Time *mono_time_given, uint32_t friendnumber, m_cb *mcb, void *mcb_user_data)
{
    int i = 0;
    BWController *retu = (BWController *)calloc(sizeof(struct BWController_s), 1);

    retu->mcb = mcb;
    retu->mcb_user_data = mcb_user_data;
    retu->friend_number = friendnumber;
    retu->bwc_mono_time = mono_time_given;
    uint64_t now = current_time_monotonic(retu->bwc_mono_time);
    retu->cycle.last_sent_timestamp = now;
    retu->cycle.last_refresh_timestamp = now;
    retu->tox = tox;

    retu->cycle.lost = 0;
    retu->cycle.recv = 0;
    retu->packet_loss_counted_cycles = 0;

    // register callback (should be lossless not lossy, to actually reach the other side)
    tox_callback_friend_lossy_packet_per_pktid(tox, bwc_handle_data, BWC_PACKET_ID);

    return retu;
}

void bwc_kill(BWController *bwc)
{
    if (!bwc) {
        return;
    }

    // UN-register callback (should be lossless not lossy, to actually reach the other side)
    tox_callback_friend_lossy_packet_per_pktid(bwc->tox, NULL, BWC_PACKET_ID);

    free(bwc);
}

void bwc_add_lost_v3(BWController *bwc, uint32_t bytes_lost, bool dummy)
{
    if (!bwc) {
        return;
    }

    // LOGGER_DEBUG(bwc->m->log, "BWC lost(1): %d", (int)bytes_lost);

    bwc->cycle.lost = bwc->cycle.lost + bytes_lost;
    send_update(bwc, dummy);
}


void bwc_add_recv(BWController *bwc, uint32_t recv_bytes)
{
    if (!bwc) {
        return;
    }

    ++bwc->packet_loss_counted_cycles;
    bwc->cycle.recv = bwc->cycle.recv + recv_bytes;
    send_update(bwc, false);
}


void send_update(BWController *bwc, bool dummy)
{
    if (current_time_monotonic(bwc->bwc_mono_time) - bwc->cycle.last_sent_timestamp > BWC_SEND_INTERVAL_MS)
    {
        bwc->packet_loss_counted_cycles = 0;

        if ((bwc->cycle.recv + bwc->cycle.lost) > 0) {
            if (bwc->cycle.lost > 0) {
                LOGGER_DEBUG(bwc->m->log, "%p Sent update rcv: %u lost: %u percent: %f %%",
                             (void *)bwc, bwc->cycle.recv, bwc->cycle.lost,
                             (float)(((float) bwc->cycle.lost / (bwc->cycle.recv + bwc->cycle.lost)) * 100.0f));
            }
        }

        uint8_t bwc_packet[sizeof(struct BWCMessage) + 1];
        struct BWCMessage *msg = (struct BWCMessage *)(bwc_packet + 1);

        bwc_packet[0] = BWC_PACKET_ID; // set packet ID
        msg->lost = net_htonl(bwc->cycle.lost);
        msg->recv = net_htonl(bwc->cycle.recv);

        if (-1 == bwc_send_custom_lossy_packet(bwc->tox, bwc->friend_number, bwc_packet, sizeof(bwc_packet))) {
            LOGGER_WARNING(bwc->m->log, "BWC send failed (len: %zu)! std error: %s", sizeof(bwc_packet), strerror(errno));
        }
        else
        {
            bwc->cycle.last_sent_timestamp = current_time_monotonic(bwc->bwc_mono_time);

            bwc->cycle.lost = 0;
            bwc->cycle.recv = 0;
        }
    }
}

static int on_update(BWController *bwc, const struct BWCMessage *msg)
{
    //LOGGER_DEBUG(bwc->m->log, "%p Got update from peer", (void *)bwc);

    /* Peers sent update too soon */
    if ((bwc->cycle.last_recv_timestamp + (BWC_SEND_INTERVAL_MS / 2)) > current_time_monotonic(bwc->bwc_mono_time)) {
        //LOGGER_DEBUG(bwc->m->log, "%p Rejecting extra update", (void *)bwc);
        return -1;
    }

    bwc->cycle.last_recv_timestamp = current_time_monotonic(bwc->bwc_mono_time);

    const uint32_t recv = msg->recv;
    const uint32_t lost = msg->lost;

    //LOGGER_DEBUG(bwc->m->log, "recved: %u lost: %u", recv, lost);

    if (bwc->mcb) {

        if ((recv + lost) > 0) {
            //LOGGER_DEBUG(bwc->m->log, "recved: %u lost: %u percentage: %f %%", recv, lost,
            //             (float)(((float) lost / (recv + lost)) * 100.0f));

            bwc->mcb(bwc, bwc->friend_number,
                     ((float) lost / (recv + lost)),
                     bwc->mcb_user_data);
        } else {
            bwc->mcb(bwc, bwc->friend_number,
                     0,
                     bwc->mcb_user_data);
        }
    }

    return 0;
}

/* !!hack!! TODO:fix me */
void *call_get(void *av, uint32_t friend_number);
BWController *bwc_controller_get(void *call);
/* !!hack!! TODO:fix me */


void bwc_handle_data(Tox *tox, uint32_t friendnumber, const uint8_t *data, size_t length, void *dummy)
{
    if (sizeof(struct BWCMessage) != (length - 1)) {
        return;
    }

    /* get BWController object from Tox and friend number */
    void *toxav = NULL;
    tox_get_av_object(tox, (void **)(&toxav));

    if (!toxav) {
        return;
    }

    void *call = NULL;
    call = (void *)call_get(toxav, friendnumber);

    if (!call) {
        return;
    }
    /* get Call object from Tox and friend number */


    BWController *bwc = NULL;
    bwc = bwc_controller_get(call);

    if (!bwc) {
        LOGGER_API_WARNING(tox, "No session!");
        return;
    }

    size_t offset = 1;  // Ignore packet id.
    struct BWCMessage msg;
    offset += net_unpack_u32(data + offset, &msg.lost);
    offset += net_unpack_u32(data + offset, &msg.recv);
    assert(offset == length);

    on_update(bwc, &msg);
}

/*
 * return -1 on failure, 0 on success
 *
 */
static int bwc_send_custom_lossy_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossy_packet(tox, friendnumber, data, (size_t)length, &error);

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 0;
    }

    return -1;
}

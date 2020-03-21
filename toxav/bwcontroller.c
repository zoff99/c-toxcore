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
    uint32_t friend_number;
    BWCCycle cycle;
    Mono_Time* bwc_mono_time;
    uint32_t packet_loss_counted_cycles;
};

struct BWCMessage {
    uint32_t lost;
    uint32_t recv;
};


int bwc_handle_data(Tox *tox, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object);
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

    retu->cycle.lost = 0;
    retu->cycle.recv = 0;
    retu->packet_loss_counted_cycles = 0;

    /*XYZ*/ //m_callback_rtp_packet(m, friendnumber, BWC_PACKET_ID, bwc_handle_data, retu);

    return retu;
}

void bwc_kill(BWController *bwc)
{
    if (!bwc) {
        return;
    }

    /*XYZ*/ //m_callback_rtp_packet(bwc->m, bwc->friend_number, BWC_PACKET_ID, NULL, NULL);

    free(bwc);
}

void bwc_add_lost_v3(BWController *bwc, uint32_t bytes_lost, bool dummy)
{
    if (!bwc) {
        return;
    }

    LOGGER_DEBUG(bwc->m->log, "BWC lost(1): %d", (int)bytes_lost);

    bwc->cycle.lost = bwc->cycle.lost + bytes_lost;
    /*XYZ*/ //send_update(bwc, dummy);
}


void bwc_add_recv(BWController *bwc, uint32_t recv_bytes)
{
    if (!bwc) {
        return;
    }

    ++bwc->packet_loss_counted_cycles;
    bwc->cycle.recv = bwc->cycle.recv + recv_bytes;
    /*XYZ*/ //send_update(bwc, false);
}


void send_update(BWController *bwc, bool dummy)
{
}

#if 0
static int on_update(BWController *bwc, const struct BWCMessage *msg)
{
    return 0;
}
#endif

int bwc_handle_data(Tox *tox, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object)
{
    return 0;
}


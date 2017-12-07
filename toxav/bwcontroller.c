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
#include "../toxcore/util.h"

#include <assert.h>
#include <errno.h>

#define BWC_PACKET_ID 196
#define BWC_SEND_INTERVAL_MS 1000
#define BWC_REFRESH_INTERVAL_MS 10000
#define BWC_AVG_PKT_COUNT 20

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

typedef struct BWCRcvPkt {
    uint32_t packet_length_array[BWC_AVG_PKT_COUNT];
    RingBuffer *rb;
} BWCRcvPkt;

struct BWController_s {
    m_cb *mcb;
    void *mcb_user_data;

    Messenger *m;
    uint32_t friend_number;

    struct {
        uint32_t lru; /* Last recv update time stamp */
        uint32_t lsu; /* Last sent update time stamp */
        uint32_t lfu; /* Last refresh time stamp */

        uint32_t lost;
        uint32_t recv;
    } cycle;

    struct {
        uint32_t rb_s[BWC_AVG_PKT_COUNT];
        RingBuffer *rb;
    } rcvpkt; /* To calculate average received packet */
};

int bwc_handle_data(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object);
void send_update(BWController *bwc);

BWController *bwc_new(Messenger *m, uint32_t friendnumber, m_cb *mcb, void *mcb_user_data)
{
    BWController *retu = (BWController *)calloc(sizeof(struct BWController_s), 1);

    retu->mcb = mcb;
    retu->mcb_user_data = mcb_user_data;
    retu->m = m;
    retu->friend_number = friendnumber;
    retu->cycle.lsu = retu->cycle.lfu = current_time_monotonic();
    retu->rcvpkt.rb = rb_new(BWC_AVG_PKT_COUNT);

    /* Fill with zeros */
    int i = 0;

    for (; i < BWC_AVG_PKT_COUNT; i ++) {
        rb_write(retu->rcvpkt.rb, retu->rcvpkt.rb_s + i, 0);
    }

    m_callback_rtp_packet(m, friendnumber, BWC_PACKET_ID, bwc_handle_data, retu);

    return retu;
}

void bwc_kill(BWController *bwc)
{
    if (!bwc) {
        return;
    }

    m_callback_rtp_packet(bwc->m, bwc->friend_number, BWC_PACKET_ID, NULL, NULL);

    rb_kill(bwc->rcvpkt.rb);
    free(bwc);
}

void bwc_feed_avg(BWController *bwc, uint32_t bytes)
{
    uint32_t *p;
    uint8_t dummy;

    rb_read(bwc->rcvpkt.rb, (void **) &p, &dummy);
    *p = bytes;
    rb_write(bwc->rcvpkt.rb, p, 0);
}

void bwc_add_lost(BWController *bwc, uint32_t bytes)
{
    if (!bwc) {
        return;
    }

    if (!bytes) {
        uint32_t *t_avg[BWC_AVG_PKT_COUNT], c = 1;

        rb_data(bwc->rcvpkt.rb, (void **) t_avg);

        int i = 0;

        for (; i < BWC_AVG_PKT_COUNT; i ++) {
            bytes += *(t_avg[i]);

            if (*(t_avg[i])) {
                c++;
            }
        }

        bytes /= c;
    }

    bwc->cycle.lost += bytes;
    send_update(bwc);
}

void bwc_add_recv(BWController *bwc, uint32_t bytes)
{
    if (!bwc || !bytes) {
        return;
    }

    bwc->cycle.recv += bytes;
    send_update(bwc);
}


struct BWCMessage {
    uint32_t lost;
    uint32_t recv;
};

void send_update(BWController *bwc)
{
    if (current_time_monotonic() - bwc->cycle.lfu > BWC_REFRESH_INTERVAL_MS) {

        bwc->cycle.lost /= 10;
        bwc->cycle.recv /= 10;
        bwc->cycle.lfu = current_time_monotonic();

    } else if (current_time_monotonic() - bwc->cycle.lsu > BWC_SEND_INTERVAL_MS) {

        if (bwc->cycle.lost) {
            LOGGER_DEBUG(bwc->m->log, "%p Sent update rcv: %u lost: %u percent: %f %%",
                         (void *)bwc, bwc->cycle.recv, bwc->cycle.lost,
                         (((double) bwc->cycle.lost / (bwc->cycle.recv + bwc->cycle.lost)) * 100.0));
            uint8_t bwc_packet[sizeof(struct BWCMessage) + 1];
            size_t offset = 0;

            bwc_packet[offset] = BWC_PACKET_ID; // set packet ID
            ++offset;

            offset += net_pack_u32(bwc_packet + offset, bwc->cycle.lost);
            offset += net_pack_u32(bwc_packet + offset, bwc->cycle.recv);
            assert(offset == sizeof(bwc_packet));

            if (m_send_custom_lossy_packet(bwc->m, bwc->friend_number, bwc_packet, sizeof(bwc_packet)) == -1) {
                const char *netstrerror = net_new_strerror(net_error());
                LOGGER_WARNING(bwc->m->log, "BWC send failed (len: %u)! std error: %s, net error %s",
                               (unsigned)sizeof(bwc_packet), strerror(errno), netstrerror);
                net_kill_strerror(netstrerror);
            }
        }

        bwc->cycle.lsu = current_time_monotonic();
    }
}

static int on_update(BWController *bwc, const struct BWCMessage *msg)
{
    LOGGER_DEBUG(bwc->m->log, "%p Got update from peer", bwc);

    /* Peer must respect time boundary */
    if (current_time_monotonic() < bwc->cycle.lru + BWC_SEND_INTERVAL_MS) {
        LOGGER_INFO(bwc->m->log, "%p Rejecting extra update", bwc);
        return -1;
    }

    bwc->cycle.lru = current_time_monotonic();

    const uint32_t recv = msg->recv;
    const uint32_t lost = msg->lost;

    LOGGER_INFO(bwc->m->log, "recved: %u lost: %u", recv, lost);

    if (lost && bwc->mcb) {

        LOGGER_INFO(bwc->m->log, "recved: %u lost: %u percentage: %f %", recv, lost, (float)( ((float) lost / (recv + lost)) * 100.0f) );

        bwc->mcb(bwc, bwc->friend_number,
                 ((float) lost / (recv + lost)),
                 bwc->mcb_user_data);
    }

    return 0;
}

int bwc_handle_data(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object)
{
    if (length - 1 != sizeof(struct BWCMessage)) {
        return -1;
    }

    size_t offset = 1;  // Ignore packet id.
    struct BWCMessage msg;
    offset += net_unpack_u32(data + offset, &msg.lost);
    offset += net_unpack_u32(data + offset, &msg.recv);
    assert(offset == length);

    return on_update((BWController *)object, &msg);
}

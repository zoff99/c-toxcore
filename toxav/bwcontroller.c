/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "bwcontroller.h"

#include "ring_buffer.h"

#include "../toxcore/logger.h"
#include "../toxcore/util.h"

/*
 * Zoff: disable logging in ToxAV for now
 */
static void dummy()
{
}

#undef LOGGER_DEBUG
#define LOGGER_DEBUG(log, ...) dummy()
// #undef LOGGER_INFO
// #define LOGGER_INFO(log, ...) dummy()

#define BWC_PACKET_ID 196
#define BWC_SEND_INTERVAL_MS (1000)     /* 1s  */
#define BWC_REFRESH_INTERVAL_MS (10000) /* 10s */
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

    uint32_t friend_number;

    struct {
        uint32_t last_recv_timestamp; /* Last recv update time stamp */
        uint32_t last_sent_timestamp; /* Last sent update time stamp */
        uint32_t last_refresh_timestamp; /* Last refresh time stamp */

        uint32_t lost;
        uint32_t recv;
    } cycle;

    uint32_t packet_loss_counted_cycles;
    Mono_Time *bwc_mono_time;
};

struct BWCMessage {
    uint32_t lost;
    uint32_t recv;
};

// static int bwc_handle_data(Tox *t, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object);
static void send_update(BWController *bwc);

#if 0
/*
 * return -1 on failure, 0 on success
 *
 */
int bwc_send_custom_lossy_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossy_packet(tox, friendnumber, data, (size_t)length, &error);

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 0;
    }

    return -1;
}
#endif

BWController *bwc_new(Tox *t, uint32_t friendnumber, m_cb *mcb, void *mcb_user_data, Mono_Time *toxav_given_mono_time)
{
    BWController *retu = (BWController *)calloc(sizeof(struct BWController_s), 1);

    LOGGER_WARNING(m->log, "BWC: new");

    retu->mcb = mcb;
    retu->mcb_user_data = mcb_user_data;
    retu->friend_number = friendnumber;
    retu->bwc_mono_time = toxav_given_mono_time;
    uint64_t now = current_time_monotonic(toxav_given_mono_time);
    retu->cycle.last_sent_timestamp = now;
    retu->cycle.last_refresh_timestamp = now;
    retu->rcvpkt.rb = rb_new(BWC_AVG_PKT_COUNT);

    retu->cycle.lost = 0;
    retu->cycle.recv = 0;

    /* Fill with zeros */
    int i = 0;

    for (i = 0; i < BWC_AVG_PKT_COUNT; i++) {
        uint32_t *j = (retu->rcvpkt.packet_length_array + i);
        *j = 0;
        rb_write(retu->rcvpkt.rb, j, 0);
    }

    //*PP*// m_callback_rtp_packet(m, friendnumber, BWC_PACKET_ID, bwc_handle_data, retu);
    return retu;
}

void bwc_kill(BWController *bwc)
{
    if (!bwc) {
        return;
    }

    //*PP*// m_callback_rtp_packet(bwc->m, bwc->friend_number, BWC_PACKET_ID, nullptr, nullptr);
    rb_kill(bwc->rcvpkt.rb);
    free(bwc);
}

void bwc_feed_avg(BWController *bwc, uint32_t bytes)
{
    // DISABLE
    return;

    uint32_t *packet_length;
    uint8_t dummy;

    rb_read(bwc->rcvpkt.rb, (void **) &packet_length, &dummy);
    *packet_length = bytes;
    rb_write(bwc->rcvpkt.rb, packet_length, 0);
}

/*
 * this function name is confusing
 * it should be called for every packet that has lost bytes, and called with how many bytes are ok
 */
void bwc_add_lost(BWController *bwc, uint32_t bytes_received_ok)
{
    if (!bwc) {
        return;
    }

    // DISABLE
    return;

    if (!bytes_received_ok) {
        LOGGER_WARNING(bwc->m->log, "BWC lost(1): %d", (int)bytes_received_ok);

        uint32_t *avg_packet_length_array[BWC_AVG_PKT_COUNT];
        uint32_t count = 1;

        rb_data(bwc->rcvpkt.rb, (void **)avg_packet_length_array);

        int i = 0;

        for (i = 0; i < BWC_AVG_PKT_COUNT; i ++) {
            bytes_received_ok = bytes_received_ok + *(avg_packet_length_array[i]);

            if (*(avg_packet_length_array[i])) {
                count++;
            }
        }

        LOGGER_WARNING(bwc->m->log, "BWC lost(2): %d count: %d", (int)bytes_received_ok, (int)count);

        bytes_received_ok = bytes_received_ok / count;

        LOGGER_WARNING(bwc->m->log, "BWC lost(3): %d", (int)bytes_received_ok);
    }

    bwc->cycle.lost = bwc->cycle.lost + bytes_received_ok;
    send_update(bwc);
}

void bwc_add_lost_v3(BWController *bwc, uint32_t bytes_lost)
{
    if (!bwc) {
        return;
    }

    if (bytes_lost > 0) {
        LOGGER_DEBUG(bwc->m->log, "BWC lost(1): %d", (int)bytes_lost);

        bwc->cycle.lost = bwc->cycle.lost + bytes_lost;
        send_update(bwc);
    }
}

static void send_update(BWController *bwc)
{
    if (bwc->packet_loss_counted_cycles > BWC_AVG_LOSS_OVER_CYCLES_COUNT &&
            current_time_monotonic(bwc->bwc_mono_time) - bwc->cycle.last_sent_timestamp > BWC_SEND_INTERVAL_MS) {
        bwc->packet_loss_counted_cycles = 0;

        if (bwc->cycle.lost) {
            LOGGER_INFO(bwc->m->log, "%p Sent update rcv: %u lost: %u percent: %f %%",
                        bwc, bwc->cycle.recv, bwc->cycle.lost,
                        (float)(((float) bwc->cycle.lost / (bwc->cycle.recv + bwc->cycle.lost)) * 100.0f));

            uint8_t bwc_packet[sizeof(struct BWCMessage) + 1];
            struct BWCMessage *msg = (struct BWCMessage *)(bwc_packet + 1);

            bwc_packet[0] = BWC_PACKET_ID; // set packet ID
            msg->lost = net_htonl(bwc->cycle.lost);
            msg->recv = net_htonl(bwc->cycle.recv);

#if 0

            if (bwc_send_custom_lossy_packet(bwc->m, bwc->friend_number, bwc_packet, sizeof(bwc_packet)) == -1) {
                const char *netstrerror = net_new_strerror(net_error());
                LOGGER_WARNING(bwc->m->log, "BWC send failed (len: %u)! std error: %s, net error %s",
                               (unsigned)sizeof(bwc_packet), strerror(errno), netstrerror);
                net_kill_strerror(netstrerror);
            }

#endif
        }

        bwc->cycle.last_sent_timestamp = current_time_monotonic(bwc->bwc_mono_time);
        bwc->cycle.lost = 0;
        bwc->cycle.recv = 0;
    }
}

#if 0
static int on_update(BWController *bwc, const struct BWCMessage *msg)
{
    LOGGER_DEBUG(bwc->m->log, "%p Got update from peer", bwc);

    /* Peers sent update too soon */
    if (bwc->cycle.last_recv_timestamp + BWC_SEND_INTERVAL_MS > current_time_monotonic(bwc->bwc_mono_time)) {
        LOGGER_INFO(bwc->m->log, "%p Rejecting extra update", (void *)bwc);
        return -1;
    }

    bwc->cycle.last_recv_timestamp = current_time_monotonic(bwc->bwc_mono_time);

    const uint32_t recv = msg->recv;
    const uint32_t lost = msg->lost;

    // LOGGER_INFO(bwc->m->log, "recved: %u lost: %u", recv, lost);

    if (lost && bwc->mcb) {

        LOGGER_INFO(bwc->m->log, "recved: %u lost: %u percentage: %f %%", recv, lost,
                    (float)(((float) lost / (recv + lost)) * 100.0f));

        bwc->mcb(bwc, bwc->friend_number,
                 ((float) lost / (recv + lost)),
                 bwc->mcb_user_data);
    }

    return 0;
}
#endif

#if 0
static int bwc_handle_data(Tox *t, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object)
{
    if (sizeof(struct BWCMessage) != (length - 1)) {
        return -1;
    }

    size_t offset = 1;  // Ignore packet id.
    struct BWCMessage msg;
    offset += net_unpack_u32(data + offset, &msg.lost);
    offset += net_unpack_u32(data + offset, &msg.recv);
    assert(offset == length);

    return on_update((BWController *)object, &msg);
}
#endif

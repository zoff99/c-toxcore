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

#include "rtp.h"

#include "bwcontroller.h"

#include "../toxcore/Messenger.h"
#include "../toxcore/logger.h"
#include "../toxcore/util.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>


int handle_rtp_packet(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object);

RTPSession *rtp_new(int payload_type, Messenger *m, uint32_t friendnumber,
                    BWController *bwc, void *cs,
                    int (*mcb)(void *, struct RTPMessage *))
{
    assert(mcb);
    assert(cs);
    assert(m);

    RTPSession *retu = (RTPSession *)calloc(1, sizeof(RTPSession));

    if (!retu) {
        LOGGER_WARNING(m->log, "Alloc failed! Program might misbehave!");
        return NULL;
    }

    retu->ssrc = random_int();
    retu->payload_type = payload_type;

    retu->m = m;
    retu->friend_number = friendnumber;

    /* Also set payload type as prefix */

    retu->bwc = bwc;
    retu->cs = cs;
    retu->mcb = mcb;

    if (-1 == rtp_allow_receiving(retu)) {
        LOGGER_WARNING(m->log, "Failed to start rtp receiving mode");
        free(retu);
        return NULL;
    }

    return retu;
}

void rtp_kill(RTPSession *session)
{
    if (!session) {
        return;
    }

    LOGGER_DEBUG(session->m->log, "Terminated RTP session: %p", session);

    rtp_stop_receiving(session);
    free(session);
}

int rtp_allow_receiving(RTPSession *session)
{
    if (session == NULL) {
        return -1;
    }

    if (m_callback_rtp_packet(session->m, session->friend_number, session->payload_type,
                              handle_rtp_packet, session) == -1) {
        LOGGER_WARNING(session->m->log, "Failed to register rtp receive handler");
        return -1;
    }

    LOGGER_DEBUG(session->m->log, "Started receiving on session: %p", session);
    return 0;
}

int rtp_stop_receiving(RTPSession *session)
{
    if (session == NULL) {
        return -1;
    }

    m_callback_rtp_packet(session->m, session->friend_number, session->payload_type, NULL, NULL);

    LOGGER_DEBUG(session->m->log, "Stopped receiving on session: %p", session);
    return 0;
}


int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length_v3, Logger *log)
{
    if (!session) {
        LOGGER_ERROR(log, "No session!");
        return -1;
    }

    // here the highest bits gets stripped anyway, no need to do keyframe bit magic here!
    uint16_t length = (uint16_t)length_v3;

    uint8_t is_keyframe = 0;
    uint8_t is_video_payload = 0;

    if (session->payload_type == rtp_TypeVideo)
    {
        is_video_payload = 1;
    }

    if (is_video_payload == 1)
    {
        // TOX RTP V3 --- hack to get frame type ---
        //
        // use the highest bit (bit 31) to spec. keyframe = 1 / no keyframe = 0
        // if length(31 bits) > 1FFFFFFF then use all bits for length
        // and assume its a keyframe (most likely is anyway)

        if (LOWER_31_BITS(length_v3) > 0x1FFFFFFF)
        {
            is_keyframe = 1;
        }
        else
        {
            is_keyframe = (length_v3 & (1 << 31)) != 0; // 1-> is keyframe, 0-> no keyframe
            length_v3 = LOWER_31_BITS(length_v3);
        }

        // TOX RTP V3 --- hack to get frame type ---
    }

    VLA(uint8_t, rdata, length_v3 + sizeof(struct RTPHeader) + 1);
    memset(rdata, 0, SIZEOF_VLA(rdata));

    rdata[0] = session->payload_type;

    struct RTPHeader *header = (struct RTPHeader *)(rdata + 1);

    header->ve = 2; // version
    header->pe = 0;
    header->xe = 0;
    header->cc = 0;

    header->ma = 0;
    header->pt = session->payload_type % 128;

    header->sequnum = net_htons(session->sequnum);
    header->timestamp = net_htonl(current_time_monotonic());
    header->ssrc = net_htonl(session->ssrc);

    header->cpart = 0;
    header->tlen = net_htons(length);


// Zoff -- new stuff --

    struct RTPHeaderV3 *header_v3 = (void *)header;

    header_v3->protocol_version = 3; // TOX RTP V3

    uint16_t length_safe = (uint16_t)(length_v3 && 0xFFFF);
    if (length > UINT16_MAX)
    {
        length_safe = UINT16_MAX;
    }
    // header_v3->data_length_lower = net_htons(length_safe);
    header_v3->data_length_full = net_htonl(length_v3);

    // header_v3->offset_lower = net_htons((uint16_t)(0 && 0xFFFF));
    header_v3->offset_full = net_htonl(0);

    header_v3->is_keyframe = is_keyframe;
    // TODO: bigendian ??

// Zoff -- new stuff --



    if (MAX_CRYPTO_DATA_SIZE > (length_v3 + sizeof(struct RTPHeader) + 1)) {

        /**
         * The length is lesser than the maximum allowed length (including header)
         * Send the packet in single piece.
         */

        memcpy(rdata + 1 + sizeof(struct RTPHeader), data, length_v3);

        if (-1 == m_send_custom_lossy_packet(session->m, session->friend_number, rdata, SIZEOF_VLA(rdata))) {
            LOGGER_WARNING(session->m->log, "RTP send failed (len: %d)! std error: %s", SIZEOF_VLA(rdata), strerror(errno));
        }
    } else {

        /**
         * The length is greater than the maximum allowed length (including header)
         * Send the packet in multiple pieces.
         */

        uint32_t sent = 0;
        uint16_t piece = MAX_CRYPTO_DATA_SIZE - (sizeof(struct RTPHeader) + 1);

        while ((length_v3 - sent) + sizeof(struct RTPHeader) + 1 > MAX_CRYPTO_DATA_SIZE) {
            memcpy(rdata + 1 + sizeof(struct RTPHeader), data + sent, piece);

            if (-1 == m_send_custom_lossy_packet(session->m, session->friend_number,
                                                 rdata, piece + sizeof(struct RTPHeader) + 1)) {
                LOGGER_WARNING(session->m->log, "RTP send failed (len: %d)! std error: %s",
                               piece + sizeof(struct RTPHeader) + 1, strerror(errno));
            }

            sent += piece;
            header->cpart = net_htons((uint16_t)sent);

// Zoff -- new stuff --

            header_v3->offset_full = net_htonl(sent);
            // TODO: bigendian ??

// Zoff -- new stuff --

        }

        /* Send remaining */
        piece = length - sent;

        if (piece) {
            memcpy(rdata + 1 + sizeof(struct RTPHeader), data + sent, piece);

            if (-1 == m_send_custom_lossy_packet(session->m, session->friend_number, rdata,
                                                 piece + sizeof(struct RTPHeader) + 1)) {
                LOGGER_WARNING(session->m->log, "RTP send failed (len: %d)! std error: %s",
                               piece + sizeof(struct RTPHeader) + 1, strerror(errno));
            }
        }
    }

    session->sequnum ++;
    return 0;
}


static bool chloss(const RTPSession *session, const struct RTPHeader *header)
{
    if (net_ntohl(header->timestamp) < session->rtimestamp) {
        uint16_t hosq, lost = 0;

        hosq = net_ntohs(header->sequnum);

        lost = (hosq > session->rsequnum) ?
               (session->rsequnum + 65535) - hosq :
               session->rsequnum - hosq;

        LOGGER_WARNING(session->m->log, "Lost packet");

        while (lost --)
        {
            bwc_add_lost(session->bwc , 0);
        }

        return true;
    }

    return false;
}


static struct RTPMessage *new_message(size_t allocate_len, const uint8_t *data, uint16_t data_length)
{
    assert(allocate_len >= data_length);

    struct RTPMessage *msg = (struct RTPMessage *)calloc(sizeof(struct RTPMessage) + (allocate_len - sizeof(
                                 struct RTPHeader)), 1);

    msg->len = data_length - sizeof(struct RTPHeader);
    memcpy(&msg->header, data, data_length);

    msg->header.sequnum = net_ntohs(msg->header.sequnum);
    msg->header.timestamp = net_ntohl(msg->header.timestamp);
    msg->header.ssrc = net_ntohl(msg->header.ssrc);

    msg->header.cpart = net_ntohs(msg->header.cpart);
    msg->header.tlen = net_ntohs(msg->header.tlen);

    return msg;
}


int handle_rtp_packet_v3(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object)
{
    (void) m;
    (void) friendnumber;

    RTPSession *session = (RTPSession *)object;

    /*
     *
     * session->mcb == vc_queue_message() // this function is called from here! arrgh **
     * session->mp == struct *RTPMessage
     * session->cs == call->video.second // == VCSession created by vc_new() call!
     *
     * next time please make it even more confusing!?!? arrgh **
     *
     */

    // here the packet ID byte gets stripped, why? -----------
    const uint8_t *data_orig = data;
    data++;
    length--; // this is the length of only this part of the message (brutto)
    // here the packet ID byte gets stripped, why? -----------


    const struct RTPHeaderV3 *header_v3 = (void *)data;

    uint32_t length_v3 = net_htonl(header_v3->data_length_full);
    uint32_t offset_v3 = net_htonl(header_v3->offset_full);

    LOGGER_WARNING(m->log, "-- handle_rtp_packet_v3 -- full len=%d is_keyframe=%s", (int)length_v3, ((int)header_v3->is_keyframe) ? "K" : ".");

    if (offset_v3 >= length_v3) {
        /* Never allow this case to happen */
		LOGGER_WARNING(m->log, "Never allow this case to happen");
        return -1;
    }

    if (length_v3 == (length - sizeof(struct RTPHeader)))
    {
        /* The message was sent in single part */


    }
    else
    {
        /* Multipart-message */
    }


}



int handle_rtp_packet(Messenger *m, uint32_t friendnumber, const uint8_t *data, uint16_t length, void *object)
{
    (void) m;
    (void) friendnumber;

    RTPSession *session = (RTPSession *)object;

    // here the packet ID byte gets stripped, why? -----------
    const uint8_t *data_orig = data;
    const uint32_t length_orig = length;
    data++;
    length--;
    // here the packet ID byte gets stripped, why? -----------

    if (!session || length < sizeof(struct RTPHeader)) {
        LOGGER_WARNING(m->log, "No session or invalid length of received buffer!");
        return -1;
    }


// Zoff -- new stuff --

    const struct RTPHeaderV3 *header_v3 = (void *)data;
    if (( ((uint8_t)header_v3->protocol_version) == 3) &&
        ( ((uint8_t)header_v3->pt) == rtp_TypeVideo)
        )
    {
        // use V3 only for Video payload (at the moment)
        return handle_rtp_packet_v3(m, friendnumber, data_orig, length_orig, object);
    }

// Zoff -- new stuff --




    // everything below here is protocol version 2 ------------------
    // everything below here is protocol version 2 ------------------
    // everything below here is protocol version 2 ------------------


    const struct RTPHeader *header = (const struct RTPHeader *) data;

    if (header->pt != session->payload_type % 128) {
        LOGGER_WARNING(m->log, "Invalid payload type with the session");
        return -1;
    }

    if (net_ntohs(header->cpart) >= net_ntohs(header->tlen)) {
        /* Never allow this case to happen */
        return -1;
    }

    bwc_feed_avg(session->bwc, length);

    if (net_ntohs(header->tlen) == length - sizeof(struct RTPHeader)) {
        /* The message is sent in single part */

        /* Only allow messages which have arrived in order;
         * drop late messages
         */
        if (chloss(session, header)) {
            return 0;
        }

        /* Message is not late; pick up the latest parameters */
        session->rsequnum = net_ntohs(header->sequnum);
        session->rtimestamp = net_ntohl(header->timestamp);

        bwc_add_recv(session->bwc, length);

        /* Invoke processing of active multiparted message */
        if (session->mp) {
            if (session->mcb) {
                session->mcb(session->cs, session->mp);
            } else {
                free(session->mp);
            }

            session->mp = NULL;
        }

        /* The message came in the allowed time;
         * process it only if handler for the session is present.
         */

        if (!session->mcb) {
            return 0;
        }

        return session->mcb(session->cs, new_message(length, data, length));
    }

    /* The message is sent in multiple parts */

    if (session->mp) {
        /* There are 2 possible situations in this case:
         *      1) being that we got the part of already processing message.
         *      2) being that we got the part of a new/old message.
         *
         * We handle them differently as we only allow a single multiparted
         * processing message
         */

        if (session->mp->header.sequnum == net_ntohs(header->sequnum) &&
                session->mp->header.timestamp == net_ntohl(header->timestamp)) {
            /* First case */

            /* Make sure we have enough allocated memory */
            if (session->mp->header.tlen - session->mp->len < length - sizeof(struct RTPHeader) ||
                    session->mp->header.tlen <= net_ntohs(header->cpart)) {
                /* There happened to be some corruption on the stream;
                 * continue wihtout this part
                 */
                return 0;
            }

            memcpy(session->mp->data + net_ntohs(header->cpart), data + sizeof(struct RTPHeader),
                   length - sizeof(struct RTPHeader));

            session->mp->len += length - sizeof(struct RTPHeader);

            bwc_add_recv(session->bwc, length);

            if (session->mp->len == session->mp->header.tlen) {
                /* Received a full message; now push it for the further
                 * processing.
                 */
                if (session->mcb) {
                    session->mcb(session->cs, session->mp);
                } else {
                    free(session->mp);
                }

                session->mp = NULL;
            }
        } else {
            /* Second case */

            if (session->mp->header.timestamp > net_ntohl(header->timestamp)) {
                /* The received message part is from the old message;
                 * discard it.
                 */
                return 0;
            }

            /* Measure missing parts of the old message */
            bwc_add_lost(session->bwc,
                         (session->mp->header.tlen - session->mp->len) +

                         /* Must account sizes of rtp headers too */
                         ((session->mp->header.tlen - session->mp->len) /
                          MAX_CRYPTO_DATA_SIZE) * sizeof(struct RTPHeader));

            /* Push the previous message for processing */
            if (session->mcb) {
                session->mcb(session->cs, session->mp);
            } else {
                free(session->mp);
            }

            session->mp = NULL;
            goto NEW_MULTIPARTED;
        }
    } else {
        /* In this case threat the message as if it was received in order
         */

        /* This is also a point for new multiparted messages */
NEW_MULTIPARTED:

        /* Only allow messages which have arrived in order;
         * drop late messages
         */
        session->mp = new_message(&header, header.data_length_lower, data + RTP_HEADER_SIZE, length - RTP_HEADER_SIZE);
        memmove(session->mp->data + header.offset_lower, session->mp->data, session->mp->len);
    }

    return 0;
}

size_t rtp_header_pack(uint8_t *const rdata, const struct RTPHeader *header)
{
    uint8_t *p = rdata;
    *p = (header->ve & 3) << 6
         | (header->pe & 1) << 5
         | (header->xe & 1) << 4
         | (header->cc & 0xf);
    ++p;
    *p = (header->ma & 1) << 7
         | (header->pt & 0x7f);
    ++p;

    p += net_pack_u16(p, header->sequnum);
    p += net_pack_u32(p, header->timestamp);
    p += net_pack_u32(p, header->ssrc);
    p += net_pack_u64(p, header->flags);
    p += net_pack_u32(p, header->offset_full);
    p += net_pack_u32(p, header->data_length_full);
    p += net_pack_u32(p, header->received_length_full);

    for (size_t i = 0; i < RTP_PADDING_FIELDS; ++i) {
        p += net_pack_u32(p, 0);
    }

    p += net_pack_u16(p, header->offset_lower);
    p += net_pack_u16(p, header->data_length_lower);
    assert(p == rdata + RTP_HEADER_SIZE);
    return p - rdata;
}

size_t rtp_header_unpack(const uint8_t *data, struct RTPHeader *header)
{
    const uint8_t *p = data;
    header->ve = (*p >> 6) & 3;
    header->pe = (*p >> 5) & 1;
    header->xe = (*p >> 4) & 1;
    header->cc = *p & 0xf;
    ++p;

    header->ma = (*p >> 7) & 1;
    header->pt = *p & 0x7f;
    ++p;

    p += net_unpack_u16(p, &header->sequnum);
    p += net_unpack_u32(p, &header->timestamp);
    p += net_unpack_u32(p, &header->ssrc);
    p += net_unpack_u64(p, &header->flags);
    p += net_unpack_u32(p, &header->offset_full);
    p += net_unpack_u32(p, &header->data_length_full);
    p += net_unpack_u32(p, &header->received_length_full);

    p += sizeof(uint32_t) * RTP_PADDING_FIELDS;

    p += net_unpack_u16(p, &header->offset_lower);
    p += net_unpack_u16(p, &header->data_length_lower);
    assert(p == data + RTP_HEADER_SIZE);
    return p - data;
}

RTPSession *rtp_new(int payload_type, Messenger *m, uint32_t friendnumber,
                    BWController *bwc, void *cs, rtp_m_cb *mcb)
{
    assert(mcb != nullptr);
    assert(cs != nullptr);
    assert(m != nullptr);

    RTPSession *session = (RTPSession *)calloc(1, sizeof(RTPSession));

    if (!session) {
        LOGGER_WARNING(m->log, "Alloc failed! Program might misbehave!");
        return nullptr;
    }

    session->work_buffer_list = (struct RTPWorkBufferList *)calloc(1, sizeof(struct RTPWorkBufferList));

    if (session->work_buffer_list == nullptr) {
        LOGGER_ERROR(m->log, "out of memory while allocating work buffer list");
        free(session);
        return nullptr;
    }

    // First entry is free.
    session->work_buffer_list->next_free_entry = 0;

    session->ssrc = payload_type == RTP_TYPE_VIDEO ? 0 : random_u32();
    session->payload_type = payload_type;
    session->m = m;
    session->friend_number = friendnumber;

    // set NULL just in case
    session->mp = nullptr;
    session->first_packets_counter = 1;

    /* Also set payload type as prefix */
    session->bwc = bwc;
    session->cs = cs;
    session->mcb = mcb;

    if (-1 == rtp_allow_receiving(session)) {
        LOGGER_WARNING(m->log, "Failed to start rtp receiving mode");
        free(session->work_buffer_list);
        free(session);
        return nullptr;
    }

    return session;
}

void rtp_kill(RTPSession *session)
{
    if (!session) {
        return;
    }

    LOGGER_DEBUG(session->m->log, "Terminated RTP session: %p", (void *)session);
    rtp_stop_receiving(session);

    LOGGER_DEBUG(session->m->log, "Terminated RTP session V3 work_buffer_list->next_free_entry: %d",
                 (int)session->work_buffer_list->next_free_entry);

    free(session->work_buffer_list);
    free(session);
}

int rtp_allow_receiving(RTPSession *session)
{
    if (session == nullptr) {
        return -1;
    }

    if (m_callback_rtp_packet(session->m, session->friend_number, session->payload_type,
                              handle_rtp_packet, session) == -1) {
        LOGGER_WARNING(session->m->log, "Failed to register rtp receive handler");
        return -1;
    }

    LOGGER_DEBUG(session->m->log, "Started receiving on session: %p", (void *)session);
    return 0;
}

int rtp_stop_receiving(RTPSession *session)
{
    if (session == nullptr) {
        return -1;
    }

    m_callback_rtp_packet(session->m, session->friend_number, session->payload_type, nullptr, nullptr);

    LOGGER_DEBUG(session->m->log, "Stopped receiving on session: %p", (void *)session);
    return 0;
}

/**
 * @param data is raw vpx data.
 * @param length is the length of the raw data.
 */
int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length,
                  bool is_keyframe, const Logger *log)
{
    if (!session) {
        LOGGER_ERROR(log, "No session!");
        return -1;
    }

    struct RTPHeader header = {0};

    header.ve = 2;  // this is unused in toxav

    header.pe = 0;

    header.xe = 0;

    header.cc = 0;

    header.ma = 0;

    header.pt = session->payload_type % 128;

    header.sequnum = session->sequnum;

    header.timestamp = current_time_monotonic(session->m->mono_time);

    header.ssrc = session->ssrc;

    header.offset_lower = 0;

    // here the highest bits gets stripped anyway, no need to do keyframe bit magic here!
    header.data_length_lower = length;

    if (session->payload_type == RTP_TYPE_VIDEO) {
        header.flags = RTP_LARGE_FRAME;
    }

    uint16_t length_safe = (uint16_t)length;

    if (length > UINT16_MAX) {
        length_safe = UINT16_MAX;
    }

    header.data_length_lower = length_safe;
    header.data_length_full = length; // without header
    header.offset_lower = 0;
    header.offset_full = 0;

    if (is_keyframe) {
        header.flags |= RTP_KEY_FRAME;
    }

        /* Message is not late; pick up the latest parameters */
        session->rsequnum = net_ntohs(header->sequnum);
        session->rtimestamp = net_ntohl(header->timestamp);

        bwc_add_recv(session->bwc, length);

        /* Again, only store message if handler is present
         */
        if (session->mcb) {
            session->mp = new_message(net_ntohs(header->tlen) + sizeof(struct RTPHeader), data, length);

            /* Reposition data if necessary */
            if (net_ntohs(header->cpart)) {
                ;
            }

            memmove(session->mp->data + net_ntohs(header->cpart), session->mp->data, session->mp->len);
        }
    }

    return 0;
}



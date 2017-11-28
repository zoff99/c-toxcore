/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "rtp.h"

#include "bwcontroller.h"

#include "../toxcore/logger.h"
#include "../toxcore/util.h"

/*
 * Zoff: disable logging in ToxAV for now
 */
#include <stdio.h>

Mono_Time *toxav_get_av_mono_time(ToxAV *toxav);
int rtp_send_custom_lossy_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length);

/**
 * The number of milliseconds we want to keep a keyframe in the buffer for,
 * even though there are no free slots for incoming frames.
 */
#define VIDEO_KEEP_KEYFRAME_IN_BUFFER_FOR_MS 15

/*
 * return -1 on failure, 0 on success
 *
 */
int rtp_send_custom_lossy_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint32_t length)
{
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossy_packet(tox, friendnumber, data, (size_t)length, &error);

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 0;
    }

    return -1;
}


// allocate_len is NOT including header!
static struct RTPMessage *new_message(Tox *tox, const struct RTPHeader *header, size_t allocate_len,
                                      const uint8_t *data,
                                      uint16_t data_length)
{
    assert(mcb);
    assert(cs);
    assert(m);

    if (msg == nullptr) {
        LOGGER_API_DEBUG(tox, "%s:%d:%s:msg=calloc(%d):NULL\n", __FILE__, __LINE__, __func__,
                         (int)(sizeof(struct RTPMessage) + allocate_len));
        return nullptr;
    } else {
        LOGGER_API_DEBUG(tox, "%s:%d:%s:msg=calloc(%d):%p\n", __FILE__, __LINE__, __func__,
                         (int)(sizeof(struct RTPMessage) + allocate_len), (void *)msg);
    }

    if (!retu) {
        LOGGER_WARNING(m->log, "Alloc failed! Program might misbehave!");
        return NULL;
    }

    retu->ssrc = random_int();
    retu->payload_type = payload_type;

    retu->m = m;
    retu->friend_number = friendnumber;

/**
 * Find the next free slot in work_buffer for the incoming data packet.
 *
 * - If the data packet belongs to a frame that's already in the work_buffer then
 *   use that slot.
 * - If there is no free slot return GET_SLOT_RESULT_DROP_OLDEST_SLOT.
 * - If the data packet is too old return GET_SLOT_RESULT_DROP_INCOMING.
 *
 * If there is a keyframe being assembled in slot 0, keep it a bit longer and
 * do not kick it out right away if all slots are full instead kick out the new
 * incoming interframe.
 */
static int8_t get_slot(Tox *tox, struct RTPWorkBufferList *wkbl, bool is_keyframe,
                       const struct RTPHeader *header, bool is_multipart)
{
    if (is_multipart) {
        // This RTP message is part of a multipart frame, so we try to find an
        // existing slot with the previous parts of the frame in it.
        for (uint8_t i = 0; i < wkbl->next_free_entry; ++i) {
            const struct RTPWorkBuffer *slot = &wkbl->work_buffer[i];

            if ((slot->buf->header.sequnum == header->sequnum) && (slot->buf->header.timestamp == header->timestamp)) {
                // Sequence number and timestamp match, so this slot belongs to
                // the same frame.
                //
                // In reality, these will almost certainly either both match or
                // both not match. Only if somehow there were 65535 frames
                // between, the timestamp will matter.
                return i;
            }
        }
    }

    // The message may or may not be part of a multipart frame.
    //
    // If it is part of a multipart frame, then this is an entirely new frame
    // for which we did not have a slot *or* the frame is so old that its slot
    // has been evicted by now.
    //
    //        |----------- time ----------->
    //        _________________
    // slot 0 |               |
    //        -----------------
    //                     _________________
    // slot 1              |               |
    //                     -----------------
    //                ____________
    // slot 2         |          | -> frame too old, drop
    //                ------------
    //
    //
    //
    //        |----------- time ----------->
    //        _________________
    // slot 0 |               |
    //        -----------------
    //                     _________________
    // slot 1              |               |
    //                     -----------------
    //                              ____________
    // slot 2                       |          | -> ok, start filling in a new slot
    //                              ------------

    // If there is a free slot:
    if (wkbl->next_free_entry < USED_RTP_WORKBUFFER_COUNT) {
        // If there is at least one filled slot:
        if (wkbl->next_free_entry > 0) {
            // Get the most recently filled slot.
            const struct RTPWorkBuffer *slot = &wkbl->work_buffer[wkbl->next_free_entry - 1];

            // If the incoming packet is older than our newest slot, drop it.
            // This is the first situation in the above diagram.
            if (slot->buf->header.timestamp > header->timestamp) {
                LOGGER_API_DEBUG(tox, "workbuffer:2:timestamp too old");
                return GET_SLOT_RESULT_DROP_INCOMING;
            }
        }

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

    // This is a key frame, it's not too old yet, so we keep it in its slot for
    // a little longer.
    LOGGER_API_INFO(tox, "keep KEYFRAME in workbuffer");
    return GET_SLOT_RESULT_DROP_INCOMING;
}

/**
 * Returns an assembled frame (as much data as we currently have for this frame,
 * some pieces may be missing)
 *
 * If there are no frames ready, we return NULL. If this function returns
 * non-NULL, it transfers ownership of the message to the caller, i.e. the
 * caller is responsible for storing it elsewhere or calling free().
 */
static struct RTPMessage *process_frame(Tox *tox, struct RTPWorkBufferList *wkbl, uint8_t slot_id)
{
    assert(wkbl->next_free_entry >= 0);

int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length_v3, Logger *log)
{
    if (!session) {
        LOGGER_ERROR(log, "No session!");
        return -1;
    }

    // Slot 0 contains a key frame, slot_id points at an interframe that is
    // relative to that key frame, so we don't use it yet.
    if (wkbl->work_buffer[0].is_keyframe && slot_id != 0) {
        LOGGER_API_DEBUG(tox, "process_frame:KEYFRAME waiting in slot 0");
        return nullptr;
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

/**
 * @param tox pointer to Tox
 * @param wkbl The list of in-progress frames, i.e. all the slots.
 * @param slot_id The slot we want to fill the data into.
 * @param is_keyframe Whether the data is part of a key frame.
 * @param header The RTP header from the incoming packet.
 * @param incoming_data The pure payload without header.
 * @param incoming_data_length The length in bytes of the incoming data payload.
 */
static bool fill_data_into_slot(Tox *tox, struct RTPWorkBufferList *wkbl, const uint8_t slot_id,
                                bool is_keyframe, const struct RTPHeader *header,
                                const uint8_t *incoming_data, uint16_t incoming_data_length)
{
    // We're either filling the data into an existing slot, or in a new one that
    // is the next free entry.
    assert(slot_id <= wkbl->next_free_entry);
    struct RTPWorkBuffer *const slot = &wkbl->work_buffer[slot_id];

    assert(header != nullptr);
    assert(is_keyframe == (bool)(header->flags & RTP_KEY_FRAME));

    if (slot->received_len == 0) {
        assert(slot->buf == nullptr);

        // No data for this slot has been received, yet, so we create a new
        // message for it with enough memory for the entire frame.
        struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + header->data_length_full);

        if (msg == nullptr) {
            LOGGER_API_ERROR(tox, "Out of memory while trying to allocate for frame of size %u\n",
                             (unsigned)header->data_length_full);
            // Out of memory: throw away the incoming data.
            return false;
        }

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
    // TODO: bigendian ??

// Zoff -- new stuff --



    if (MAX_CRYPTO_DATA_SIZE > (length_v3 + sizeof(struct RTPHeader) + 1)) {

static void update_bwc_values(RTPSession *session, const struct RTPMessage *msg)
{
    if (session->first_packets_counter < DISMISS_FIRST_LOST_VIDEO_PACKET_COUNT) {
        ++session->first_packets_counter;
    } else {
        uint32_t data_length_full = msg->header.data_length_full; // without header
        uint32_t received_length_full = msg->header.received_length_full; // without header
        bwc_add_recv(session->bwc, data_length_full);

        if (received_length_full < data_length_full) {
            LOGGER_API_DEBUG(session->tox, "BWC: full length=%u received length=%d", data_length_full, received_length_full);
            bwc_add_lost(session->bwc, (data_length_full - received_length_full));
        }
    } else {

static Mono_Time *rtp_get_mono_time_from_rtpsession(RTPSession *session)
{
    if (!session) {
        return NULL;
    }

    if (!session->toxav) {
        return NULL;
    }

    return toxav_get_av_mono_time(session->toxav);
}

/**
 * Handle a single RTP video packet.
 *
 * The packet may or may not be part of a multipart frame. This function will
 * find out and handle it appropriately.
 *
 * @param session The current RTP session with:
 *   <code>
 *   session->mcb == vc_queue_message() // this function is called from here
 *   session->mp == struct RTPMessage *
 *   session->cs == call->video.second // == VCSession created by vc_new() call
 *   </code>
 * @param header The RTP header deserialised from the packet.
 * @param incoming_data The packet data *not* header, i.e. this is the actual
 *   payload.
 * @param incoming_data_length The packet length *not* including header, i.e.
 *   this is the actual payload length.
 *
 * @return -1 on error, 0 on success.
 */
static int handle_video_packet(RTPSession *session, const struct RTPHeader *header,
                               const uint8_t *incoming_data, uint16_t incoming_data_length, const Logger *log)
{
    // Full frame length in bytes. The frame may be split into multiple packets,
    // but this value is the complete assembled frame size.
    const uint32_t full_frame_length = header->data_length_full;

    // The sender tells us whether this is a key frame.
    const bool is_keyframe = (header->flags & RTP_KEY_FRAME) != 0;

    LOGGER_API_DEBUG(session->tox, "wkbl->next_free_entry:003=%d", session->work_buffer_list->next_free_entry);

            sent += piece;
            header->cpart = net_htons((uint16_t)sent);

    /* The message was sent in single part */
    int8_t slot_id = get_slot(session->tox, session->work_buffer_list, is_keyframe, header, is_multipart);
    LOGGER_API_DEBUG(session->tox, "slot num=%d", slot_id);

            header_v3->offset_full = net_htonl(sent);
            // TODO: bigendian ??

    // get_slot said there is no free slot.
    if (slot_id == GET_SLOT_RESULT_DROP_OLDEST_SLOT) {
        LOGGER_API_DEBUG(session->tox, "there was no free slot, so we process the oldest frame");
        // We now own the frame.
        struct RTPMessage *m_new = process_frame(session->tox, session->work_buffer_list, 0);

        // The process_frame function returns NULL if there is no slot 0, i.e.
        // the work buffer list is completely empty. It can't be empty, because
        // get_slot just told us it's full, so process_frame must return non-null.
        assert(m_new != nullptr);

        LOGGER_API_DEBUG(session->tox, "-- handle_video_packet -- CALLBACK-001a b0=%d b1=%d", (int)m_new->data[0],
                         (int)m_new->data[1]);
        update_bwc_values(session, m_new);
        // Pass ownership of m_new to the callback.
        session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, m_new);
        // Now we no longer own m_new.
        m_new = nullptr;

        // Now we must have a free slot, so we either get that slot, i.e. >= 0,
        // or get told to drop the incoming packet if it's too old.
        slot_id = get_slot(session->tox, session->work_buffer_list, is_keyframe, header, /* is_multipart */false);

        if (slot_id == GET_SLOT_RESULT_DROP_INCOMING) {
            // The incoming frame is too old, so we drop it.
            return -1;
        }
    }

    // We must have a valid slot here.
    assert(slot_id >= 0);

    LOGGER_API_DEBUG(session->tox, "fill_data_into_slot.1");

    // fill in this part into the slot buffer at the correct offset
    if (!fill_data_into_slot(
                session->tox,
                session->work_buffer_list,
                slot_id,
                is_keyframe,
                header,
                incoming_data,
                incoming_data_length)) {
        // Memory allocation failed. Return error.
        return -1;
    }

    struct RTPMessage *m_new = process_frame(session->tox, session->work_buffer_list, slot_id);

    if (m_new) {
        LOGGER_API_DEBUG(session->tox, "-- handle_video_packet -- CALLBACK-003a b0=%d b1=%d", (int)m_new->data[0],
                         (int)m_new->data[1]);
        update_bwc_values(session, m_new);
        session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, m_new);

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

/* !!hack!! TODO:fix me */
void *call_get(void *av, uint32_t friend_number);
RTPSession *rtp_session_get(void *call, int payload_type);
/* !!hack!! TODO:fix me */

/**
 * receive custom lossypackets and process them. they can be incoming audio or video packets
 */
void handle_rtp_packet(Tox *tox, uint32_t friendnumber, const uint8_t *data, size_t length2, void *dummy)
{
    // TODO(Zoff): is this ok?
    uint16_t length = (uint16_t)length2;

    if (length < RTP_HEADER_SIZE + 1) {
        LOGGER_API_WARNING(tox, "Invalid length of received buffer!");
        return;
    }

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

    RTPSession *session = NULL;
    session = rtp_session_get(call, data[0]);

    if (!session) {
        LOGGER_API_WARNING(tox, "No session!");
        return;
    }

        lost = (hosq > session->rsequnum) ?
               (session->rsequnum + 65535) - hosq :
               session->rsequnum - hosq;

        fprintf(stderr, "Lost packet\n");

    if (header.pt != packet_type % 128) {
        LOGGER_API_WARNING(tox, "RTPHeader packet type and Tox protocol packet type did not agree: %d != %d",
                           header.pt, packet_type % 128);
        return;
    }

    if (header.pt != session->payload_type % 128) {
        LOGGER_API_WARNING(tox, "RTPHeader packet type does not match this session's payload type: %d != %d",
                           header.pt, session->payload_type % 128);
        return;
    }

    if (header.flags & RTP_LARGE_FRAME && header.offset_full >= header.data_length_full) {
        LOGGER_API_ERROR(tox, "Invalid video packet: frame offset (%u) >= full frame length (%u)",
                         (unsigned)header.offset_full, (unsigned)header.data_length_full);
        return;
    }

    if (header.offset_lower >= header.data_length_lower) {
        LOGGER_API_ERROR(tox, "Invalid old protocol video packet: frame offset (%u) >= full frame length (%u)",
                         (unsigned)header.offset_lower, (unsigned)header.data_length_lower);
        return;
    }

    LOGGER_API_DEBUG(tox, "header.pt %d, video %d", (uint8_t)header.pt, (RTP_TYPE_VIDEO % 128));

    // The sender uses the new large-frame capable protocol and is sending a
    // video packet.
    if ((header.flags & RTP_LARGE_FRAME) && header.pt == (RTP_TYPE_VIDEO % 128)) {
        handle_video_packet(session, &header, data + RTP_HEADER_SIZE, length - RTP_HEADER_SIZE, nullptr);
        return;
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
            session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, session->mp);
            session->mp = nullptr;
        }

        /* The message came in the allowed time;
         * process it only if handler for the session is present.
         */

        session->mp = new_message(tox, &header, length - RTP_HEADER_SIZE, data + RTP_HEADER_SIZE, length - RTP_HEADER_SIZE);
        session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, session->mp);
        session->mp = nullptr;
        return;
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
                return;
            }

            memcpy(session->mp->data + net_ntohs(header->cpart), data + sizeof(struct RTPHeader),
                   length - sizeof(struct RTPHeader));

            session->mp->len += length - sizeof(struct RTPHeader);

            bwc_add_recv(session->bwc, length);

            if (session->mp->len == session->mp->header.tlen) {
                /* Received a full message; now push it for the further
                 * processing.
                 */
                session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, session->mp);
                session->mp = nullptr;
            }
        } else {
            /* Second case */

            if (session->mp->header.timestamp > net_ntohl(header->timestamp)) {
                /* The received message part is from the old message;
                 * discard it.
                 */
                return;
            }

            /* Measure missing parts of the old message */
            bwc_add_lost(session->bwc,
                         (session->mp->header.tlen - session->mp->len) +

                         /* Must account sizes of rtp headers too */
                         ((session->mp->header.tlen - session->mp->len) /
                          MAX_CRYPTO_DATA_SIZE) * sizeof(struct RTPHeader));

            /* Push the previous message for processing */
            session->mcb(rtp_get_mono_time_from_rtpsession(session), session->cs, session->mp);

            session->mp = NULL;
            goto NEW_MULTIPARTED;
        }
    } else {
        /* In this case treat the message as if it was received in order
         */

        /* This is also a point for new multiparted messages */
NEW_MULTIPARTED:

        /* Only allow messages which have arrived in order;
         * drop late messages
         */
        session->mp = new_message(tox, &header, header.data_length_lower, data + RTP_HEADER_SIZE, length - RTP_HEADER_SIZE);
        memmove(session->mp->data + header.offset_lower, session->mp->data, session->mp->len);
    }

    return;
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

RTPSession *rtp_new(int payload_type, Tox *tox, ToxAV *toxav, uint32_t friendnumber,
                    BWController *bwc, void *cs, rtp_m_cb *mcb)
{
    assert(mcb != nullptr);
    assert(cs != nullptr);

    RTPSession *session = (RTPSession *)calloc(1, sizeof(RTPSession));

    if (!session) {
        LOGGER_API_WARNING(tox, "Alloc failed! Program might misbehave!");
        return nullptr;
    }

    session->work_buffer_list = (struct RTPWorkBufferList *)calloc(1, sizeof(struct RTPWorkBufferList));

    if (session->work_buffer_list == nullptr) {
        LOGGER_API_ERROR(tox, "out of memory while allocating work buffer list");
        free(session);
        return nullptr;
    }

    // First entry is free.
    session->work_buffer_list->next_free_entry = 0;

    session->ssrc = payload_type == RTP_TYPE_VIDEO ? 0 : random_u32(); // Zoff: what is this??
    session->payload_type = payload_type;
    // session->m = m;
    session->tox = tox;
    session->toxav = toxav;
    session->friend_number = friendnumber;

    // set NULL just in case
    session->mp = nullptr;
    session->first_packets_counter = 1;

    /* Also set payload type as prefix */
    session->bwc = bwc;
    session->cs = cs;
    session->mcb = mcb;

    rtp_allow_receiving(tox, session);

    return session;
}

void rtp_kill(Tox *tox, RTPSession *session)
{
    if (!session) {
        return;
    }

    LOGGER_API_DEBUG(session->tox, "Terminated RTP session: %p", (void *)session);
    rtp_stop_receiving(tox, session);

    LOGGER_API_DEBUG(session->tox, "Terminated RTP session V3 work_buffer_list->next_free_entry: %d",
                     (int)session->work_buffer_list->next_free_entry);

    free(session->work_buffer_list);
    free(session);
}

void rtp_allow_receiving(Tox *tox, RTPSession *session)
{
    if (session) {
        // register callback
        tox_callback_friend_lossy_packet_per_pktid(tox, handle_rtp_packet, session->payload_type);
    }
}

void rtp_stop_receiving(Tox *tox, RTPSession *session)
{
    if (session) {
        // UN-register callback
        tox_callback_friend_lossy_packet_per_pktid(tox, handle_rtp_packet, session->payload_type);
    }
}

/**
 * @param data is raw vpx data.
 * @param length is the length of the raw data.
 */
int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length,
                  bool is_keyframe, const Logger *log)
{
    if (!session) {
        LOGGER_API_ERROR(session->tox, "No session!");
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

    header.timestamp = current_time_monotonic(rtp_get_mono_time_from_rtpsession(session));

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

        if (-1 == rtp_send_custom_lossy_packet(session->tox, session->friend_number, rdata, SIZEOF_VLA(rdata))) {
            const char *netstrerror = net_new_strerror(net_error());
            LOGGER_API_WARNING(session->tox, "RTP send failed (len: %u)! std error: %s, net error: %s",
                               (unsigned)SIZEOF_VLA(rdata), strerror(errno), netstrerror);
            net_kill_strerror(netstrerror);
        }
    } else {
        /**
         * The length is greater than the maximum allowed length (including header)
         * Send the packet in multiple pieces.
         */
        if (session->mcb) {
            session->mp = new_message(net_ntohs(header->tlen) + sizeof(struct RTPHeader), data, length);

        while ((length - sent) + RTP_HEADER_SIZE + 1 > MAX_CRYPTO_DATA_SIZE) {
            rtp_header_pack(rdata + 1, &header);
            memcpy(rdata + 1 + RTP_HEADER_SIZE, data + sent, piece);

            if (-1 == rtp_send_custom_lossy_packet(session->tox, session->friend_number,
                                                   rdata, piece + RTP_HEADER_SIZE + 1)) {
                const char *netstrerror = net_new_strerror(net_error());
                LOGGER_API_WARNING(session->tox, "RTP send failed (len: %d)! std error: %s, net error: %s",
                                   piece + RTP_HEADER_SIZE + 1, strerror(errno), netstrerror);
                net_kill_strerror(netstrerror);
            }

            sent += piece;
            header.offset_lower = sent;
            header.offset_full = sent; // raw data offset, without any header
        }

        /* Send remaining */
        piece = length - sent;

        if (piece) {
            rtp_header_pack(rdata + 1, &header);
            memcpy(rdata + 1 + RTP_HEADER_SIZE, data + sent, piece);

            if (-1 == rtp_send_custom_lossy_packet(session->tox, session->friend_number, rdata,
                                                   piece + RTP_HEADER_SIZE + 1)) {
                const char *netstrerror = net_new_strerror(net_error());
                LOGGER_API_WARNING(session->tox, "RTP send failed (len: %d)! std error: %s, net error: %s",
                                   piece + RTP_HEADER_SIZE + 1, strerror(errno), netstrerror);
                net_kill_strerror(netstrerror);
            }
        }
    }

    return 0;
}

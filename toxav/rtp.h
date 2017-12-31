/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifndef C_TOXCORE_TOXAV_RTP_H
#define C_TOXCORE_TOXAV_RTP_H

#include "bwcontroller.h"

#include "../toxcore/tox.h"
#include "../toxcore/logger.h"

#include <stdbool.h>

/**
 * Payload type identifier. Also used as rtp callback prefix.
 */
enum {
    rtp_TypeAudio = 192,
    rtp_TypeVideo, // = 193
};


enum {
    video_frame_type_NORMALFRAME = 0,
    video_frame_type_KEYFRAME, // = 1
};

#define VIDEO_KEEP_KEYFRAME_IN_BUFFER_FOR_MS (5)
#define USED_RTP_WORKBUFFER_COUNT (5 * 10) // correct size for fragments!!
#define VIDEO_FRAGMENT_NUM_NO_FRAG (-1)

struct RTPHeader {
    /* Standard RTP header */
#ifndef WORDS_BIGENDIAN
    uint16_t cc: 4; /* Contributing sources count */
    uint16_t xe: 1; /* Extra header */
    uint16_t pe: 1; /* Padding */
    uint16_t ve: 2; /* Version */

    uint16_t pt: 7; /* Payload type */
    uint16_t ma: 1; /* Marker */
#else
    uint16_t ve: 2; /* Version */
    uint16_t pe: 1; /* Padding */
    uint16_t xe: 1; /* Extra header */
    uint16_t cc: 4; /* Contributing sources count */

    uint16_t ma: 1; /* Marker */
    uint16_t pt: 7; /* Payload type */
#endif

    uint16_t sequnum;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t csrc[16];

#ifndef TOXAV_DEFINED
#define TOXAV_DEFINED
#undef ToxAV
typedef struct ToxAV ToxAV;
#endif /* TOXAV_DEFINED */

/**
 * A bit mask (up to 64 bits) specifying features of the current frame affecting
 * the behaviour of the decoder.
 */
typedef enum RTPFlags {
    /**
     * Support frames larger than 64KiB. The full 32 bit length and offset are
     * set in \ref RTPHeader::data_length_full and \ref RTPHeader::offset_full.
     */
    RTP_LARGE_FRAME = 1 << 0,
    /**
     * Whether the packet is part of a key frame.
     */
    RTP_KEY_FRAME = 1 << 1,
} RTPFlags;

/* Check alignment */
typedef char __fail_if_misaligned_1 [ sizeof(struct RTPHeader) == 80 ? 1 : -1 ];


// #define LOWER_31_BITS(x) (x & ((int)(1L << 31) - 1))
#define LOWER_31_BITS(x) (uint32_t)(x & 0x7fffffff)


struct RTPHeaderV3 {
#ifndef WORDS_BIGENDIAN
    uint16_t cc: 4; /* Contributing sources count */
    uint16_t is_keyframe: 1;
    uint16_t pe: 1; /* Padding */
    uint16_t protocol_version: 2; /* Version has only 2 bits! */

    uint16_t pt: 7; /* Payload type */
    uint16_t ma: 1; /* Marker */
#else
    uint16_t protocol_version: 2; /* Version has only 2 bits! */
    uint16_t pe: 1; /* Padding */
    uint16_t is_keyframe: 1;
    uint16_t cc: 4; /* Contributing sources count */

    uint16_t ma: 1; /* Marker */
    uint16_t pt: 7; /* Payload type */
#endif

    uint16_t sequnum;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t offset_full; /* Data offset of the current part */
    uint32_t data_length_full; /* data length without header, and without packet id */
    uint32_t received_length_full; /* only the receiver uses this field */
    uint64_t frame_record_timestamp; /* when was this frame actually recorded (this is a relative value!) */
    int32_t  fragment_num; /* if using fragments, this is the fragment/partition number */
    uint32_t real_frame_num; /* unused for now */
    uint32_t csrc[9];

    uint16_t offset_lower;      /* Data offset of the current part */
    uint16_t data_length_lower; /* data length without header, and without packet id */
} __attribute__((packed));

/* Check struct size */
typedef char __fail_if_size_wrong_1 [ sizeof(struct RTPHeaderV3) == 80 ? 1 : -1 ];


/* Check that V3 header is the same size as previous header */
typedef char __fail_if_size_wrong_2 [ sizeof(struct RTPHeader) == sizeof(struct RTPHeaderV3) ? 1 : -1 ];


#define USED_RTP_WORKBUFFER_COUNT 3

/**
 * One slot in the work buffer list. Represents one frame that is currently
 * being assembled.
 */
struct RTPWorkBuffer {
    /**
     * Whether this slot contains a key frame. This is true iff
     * `buf->header.flags & RTP_KEY_FRAME`.
     */
    bool is_keyframe;
    /**
     * The number of bytes received so far, regardless of which pieces. I.e. we
     * could have received the first 1000 bytes and the last 1000 bytes with
     * 4000 bytes in the middle still to come, and this number would be 2000.
     */
    uint32_t received_len;
    /**
     * The message currently being assembled.
     */
    struct RTPMessage *buf;
};

struct RTPMessage {
    uint16_t len; // Zoff: this is actually only the length of the current part of this message!

    struct RTPHeader header;
    uint8_t data[];
} __attribute__((packed));

/* Check alignment */
typedef char __fail_if_misaligned_2 [ sizeof(struct RTPMessage) == 82 ? 1 : -1 ];



struct RTPWorkBuffer {
    uint8_t frame_type;
    uint32_t received_len;
    uint32_t data_len;
    uint32_t timestamp;
    // uint64_t timestamp_v3;
    uint16_t sequnum;
    int32_t  fragment_num;
    uint8_t *buf;
};

struct RTPWorkBufferList {
    int8_t next_free_entry;
    struct RTPWorkBuffer work_buffer[USED_RTP_WORKBUFFER_COUNT];
};

#define DISMISS_FIRST_LOST_VIDEO_PACKET_COUNT (10)

/**
 * RTP control session.
 */
typedef struct {
    uint8_t  payload_type;
    uint16_t sequnum;      /* Sending sequence number */
    uint16_t rsequnum;     /* Receiving sequence number */
    uint32_t rtimestamp;
    uint32_t ssrc; //  this seems to be unused!?

    struct RTPMessage *mp; /* Expected parted message */
    struct RTPWorkBufferList *work_buffer_list;
    uint8_t  first_packets_counter; /* dismiss first few lost video packets */
    Tox *tox;
    ToxAV *toxav;
    uint32_t friend_number;

    BWController *bwc;
    void *cs;
    rtp_m_cb *mcb;
} RTPSession;


void handle_rtp_packet(Tox *tox, uint32_t friendnumber, const uint8_t *data, size_t length, void *object);

/**
 * Serialise an RTPHeader to bytes to be sent over the network.
 *
 * @param rdata A byte array of length RTP_HEADER_SIZE. Does not need to be
 *   initialised. All RTP_HEADER_SIZE bytes will be initialised after a call
 *   to this function.
 * @param header The RTPHeader to serialise.
 */
size_t rtp_header_pack(uint8_t *rdata, const struct RTPHeader *header);

/**
 * Deserialise an RTPHeader from bytes received over the network.
 *
 * @param data A byte array of length RTP_HEADER_SIZE.
 * @param header The RTPHeader to write the unpacked values to.
 */
size_t rtp_header_unpack(const uint8_t *data, struct RTPHeader *header);

RTPSession *rtp_new(int payload_type, Tox *tox, ToxAV *toxav, uint32_t friendnumber,
                    BWController *bwc, void *cs, rtp_m_cb *mcb);
void rtp_kill(Tox *tox, RTPSession *session);
void rtp_allow_receiving(Tox *tox, RTPSession *session);
void rtp_stop_receiving(Tox *tox, RTPSession *session);
/**
 * Send a frame of audio or video data, chunked in \ref RTPMessage instances.
 *
 * @param session The A/V session to send the data for.
 * @param data A byte array of length \p length.
 * @param length The number of bytes to send from @p data.
 * @param is_keyframe Whether this video frame is a key frame. If it is an
 *   audio frame, this parameter is ignored.
 */
int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length,
                  bool is_keyframe, const Logger *log);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif /* RTP_H */



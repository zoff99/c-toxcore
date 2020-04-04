/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifndef C_TOXCORE_TOXAV_RTP_H
#define C_TOXCORE_TOXAV_RTP_H

#include "bwcontroller.h"

#include "../toxcore/tox.h"
#include "../toxcore/logger.h"
#include "../toxcore/net_crypto.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOXAV_SKIP_FPS_RELEASE_AFTER_MS 13000 /* 13 seconds */


/**
 * RTPHeader serialised size in bytes.
 */
#define RTP_HEADER_SIZE 80

/**
 * Number of 32 bit padding fields between \ref RTPHeader::offset_lower and
 * everything before it.
 */
#define RTP_PADDING_FIELDS 3

#define PACKET_LOSSLESS_VIDEO 171 // DO NOT USE THIS! only for debugging!
#define PACKET_TOXAV_COMM_CHANNEL 172

/**
 * Payload type identifier. Also used as rtp callback prefix.
 */
typedef enum RTP_Type {
    RTP_TYPE_AUDIO = 192,
    RTP_TYPE_VIDEO = 193,
} RTP_Type;

#ifndef TOXAV_DEFINED
#define TOXAV_DEFINED
#undef ToxAV
typedef struct ToxAV ToxAV;
#endif /* TOXAV_DEFINED */

enum {
    video_frame_type_NORMALFRAME = 0,
    video_frame_type_KEYFRAME = 1,
};


#define USED_RTP_WORKBUFFER_COUNT 10
#define VIDEO_FRAGMENT_NUM_NO_FRAG (-1)


#define VP8E_SET_CPUUSED_VALUE (16)
/*
Codec control function to set encoder internal speed settings.
Changes in this value influences, among others, the encoder's selection of motion estimation methods.
Values greater than 0 will increase encoder speed at the expense of quality.

Note
    Valid range for VP8: -16..16
    Valid range for VP9: -8..8
 */
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

    /**
     * Whether H264 codec was used to encode this video frame
     */
    RTP_ENCODER_IS_H264 = 1 << 2,

    /**
     * Whether we have record-timestamp for this video frame
     */
    RTP_ENCODER_HAS_RECORD_TIMESTAMP = 1 << 3,

    /**
     * The orientation angle of this video frame
     *
     * possible values:
     *
     * 0   -> b0=0,b1=0
     * 90  -> b0=1,b1=0
     * 180 -> b0=0,b1=1
     * 270 -> b0=1,b1=1
     */
    RTP_ENCODER_VIDEO_ROTATION_ANGLE_BIT0 = 1 << 4,
    RTP_ENCODER_VIDEO_ROTATION_ANGLE_BIT1 = 1 << 5,

} RTPFlags;


struct RTPHeader {
    /* Standard RTP header */
    unsigned ve: 2; /* Version has only 2 bits! */ // was called "protocol_version" in V3
    unsigned pe: 1; /* Padding */
    unsigned xe: 1; /* Extra header */
    unsigned cc: 4; /* Contributing sources count */

    unsigned ma: 1; /* Marker */
    unsigned pt: 7; /* Payload type */

    uint16_t sequnum;
    uint32_t timestamp;
    uint32_t ssrc;

    /* Non-standard Tox-specific fields */

    /**
     * Bit mask of \ref RTPFlags setting features of the current frame.
     */
    uint64_t flags;

    /**
     * The full 32 bit data offset of the current data chunk. The \ref
     * offset_lower data member contains the lower 16 bits of this value. For
     * frames smaller than 64KiB, \ref offset_full and \ref offset_lower are
     * equal.
     */
    uint32_t offset_full;
    /**
     * The full 32 bit payload length without header and packet id.
     */
    uint32_t data_length_full;
    /**
     * Only the receiver uses this field (why do we have this?).
     */
    uint32_t received_length_full;


    // ---------------------------- //
    //      custom fields here      //
    // ---------------------------- //
    uint64_t frame_record_timestamp; /* when was this frame actually recorded (this is a relative value!) */
    int32_t  fragment_num; /* if using fragments, this is the fragment/partition number */
    uint32_t real_frame_num; /* unused for now */
    uint32_t encoder_bit_rate_used; /* what was the encoder bit rate used to encode this frame */
    uint32_t client_video_capture_delay_ms; /* how long did the client take to capture a video frame in ms */
    uint64_t rtp_packet_timestamp; /* when was this rtp packet sent (this is a relative value!) */
    // ---------------------------- //
    //      custom fields here      //
    // ---------------------------- //


    // ---------------------------- //
    //    dont change below here    //
    // ---------------------------- //

    /**
     * Data offset of the current part (lower bits).
     */
    uint16_t offset_lower; // used to be called "cpart"
    /**
     * Total message length (lower bits).
     */
    uint16_t data_length_lower; // used to be called "tlen"
};


struct RTPMessage {
    /**
     * This is used in the old code that doesn't deal with large frames, i.e.
     * the audio code or receiving code for old 16 bit messages. We use it to
     * record the number of bytes received so far in a multi-part message. The
     * multi-part message in the old code is stored in \ref RTPSession::mp.
     */
    uint16_t len;

    struct RTPHeader header;
    uint8_t data[];
};

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

struct RTPWorkBufferList {
    int8_t next_free_entry;
    struct RTPWorkBuffer work_buffer[USED_RTP_WORKBUFFER_COUNT];
};

#define DISMISS_FIRST_LOST_VIDEO_PACKET_COUNT 10
#define INCOMING_PACKETS_TS_ENTRIES 10

typedef int rtp_m_cb(Mono_Time *mono_time, void *cs, struct RTPMessage *msg);

/**
 * RTP control session.
 */
typedef struct RTPSession {
    uint8_t  payload_type;
    uint16_t sequnum;      /* Sending sequence number */
    uint16_t rsequnum;     /* Receiving sequence number */
    uint32_t rtimestamp;
    uint32_t ssrc; //  this seems to be unused!?
    struct RTPMessage *mp; /* Expected parted message */
    struct RTPWorkBufferList *work_buffer_list;
    uint8_t  first_packets_counter; /* dismiss first few lost video packets */
    uint32_t incoming_packets_ts[INCOMING_PACKETS_TS_ENTRIES];
    int64_t incoming_packets_ts_last_ts;
    uint16_t incoming_packets_ts_index;
    uint32_t incoming_packets_ts_average;
    int64_t incoming_rtp_packets_ts_last_local;
    int64_t incoming_rtp_packets_ts_last_remote;
    Tox *tox;
    ToxAV *toxav;
    uint32_t friend_number;
    bool rtp_receive_active;
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
void rtp_allow_receiving_mark(Tox *tox, RTPSession *session);
void rtp_stop_receiving_mark(Tox *tox, RTPSession *session);
void rtp_allow_receiving(Tox *tox);
void rtp_stop_receiving(Tox *tox);
/**
 * Send a frame of audio or video data, chunked in \ref RTPMessage instances.
 *
 * @param session The A/V session to send the data for.
 * @param data A byte array of length \p length.
 * @param length The number of bytes to send from @p data.
 * @param is_keyframe Whether this video frame is a key frame. If it is an
 *   audio frame, this parameter is ignored.
 */
int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length, bool is_keyframe,
                  uint64_t frame_record_timestamp, int32_t fragment_num,
                  uint32_t codec_used, uint32_t bit_rate_used,
                  uint32_t client_capture_delay_ms,
                  uint32_t video_frame_orientation_angle,
                  Logger *log);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // C_TOXCORE_TOXAV_RTP_H

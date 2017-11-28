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
#ifndef RTP_H
#define RTP_H

#include "bwcontroller.h"

#include "../toxcore/Messenger.h"
#include "../toxcore/logger.h"

#include <stdbool.h>

/**
 * Payload type identifier. Also used as rtp callback prefix.
 */
enum {
    rtp_TypeAudio = 192,
    rtp_TypeVideo, // = 193
};

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

    /* Non-standard TOX-specific fields */
    uint16_t cpart;/* Data offset of the current part */
    uint16_t tlen; /* Total message length */
} __attribute__((packed));

/* Check alignment */
typedef char __fail_if_misaligned_1 [ sizeof(struct RTPHeader) == 80 ? 1 : -1 ];


// #define LOWER_31_BITS(x) (x & ((int)(1L << 31) - 1))
#define LOWER_31_BITS(x) (x & 0x7fffffff)


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
    uint32_t handled_length_full; /* only the receiver uses this field */
    uint32_t csrc[13];

    uint16_t offset_lower;      /* Data offset of the current part */
    uint16_t data_length_lower; /* data length without header, and without packet id */
} __attribute__((packed));

/* Check struct size */
typedef char __fail_if_size_wrong_1 [ sizeof(struct RTPHeaderV3) == 80 ? 1 : -1 ];


/* Check that V3 header is the same size as previous header */
typedef char __fail_if_size_wrong_2 [ sizeof(struct RTPHeader) == sizeof(struct RTPHeaderV3) ? 1 : -1 ];



struct RTPMessage {
    uint16_t len; // Zoff: this is actually only the length of the current part of this message!

    struct RTPHeader header;
    uint8_t data[];
} __attribute__((packed));

/* Check alignment */
typedef char __fail_if_misaligned_2 [ sizeof(struct RTPMessage) == 82 ? 1 : -1 ];

/**
 * RTP control session.
 */
typedef struct {
    uint8_t  payload_type;
    uint16_t sequnum;      /* Sending sequence number */
    uint16_t rsequnum;     /* Receiving sequence number */
    uint32_t rtimestamp;
    uint32_t ssrc;

    struct RTPMessage *mp; /* Expected parted message */

    Messenger *m;
    uint32_t friend_number;

    BWController *bwc;
    void *cs;
    int (*mcb)(void *, struct RTPMessage *msg);
} RTPSession;


RTPSession *rtp_new(int payload_type, Messenger *m, uint32_t friendnumber,
                    BWController *bwc, void *cs,
                    int (*mcb)(void *, struct RTPMessage *));
void rtp_kill(RTPSession *session);
int rtp_allow_receiving(RTPSession *session);
int rtp_stop_receiving(RTPSession *session);
int rtp_send_data(RTPSession *session, const uint8_t *data, uint32_t length, Logger *log);

#endif /* RTP_H */

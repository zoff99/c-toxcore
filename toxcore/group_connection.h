/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2020 The TokTok team.
 * Copyright © 2015 Tox project.
 */

/*
 * An implementation of massive text only group chats.
 */

#ifndef GROUP_CONNECTION_H
#define GROUP_CONNECTION_H

#include "group_chats.h"

/* Max number of messages to store in the send/recv arrays (must fit inside an uint16) */
#define GCC_BUFFER_SIZE 8192

/* Max number of TCP relays we share with a peer */
#define GCC_MAX_TCP_SHARED_RELAYS 3

/* The time between attempts to share our TCP relays with a peer */
#define GCC_TCP_SHARED_RELAYS_TIMEOUT 300

/* The time before the direct UDP connection is considered dead */
#define GCC_UDP_DIRECT_TIMEOUT (GC_PING_INTERVAL * 2 + 2)

#define HANDSHAKE_SENDING_TIMEOUT 3

struct GC_Message_Array_Entry {
    uint8_t *data;
    uint32_t data_length;
    uint8_t  packet_type;
    uint64_t message_id;
    uint64_t time_added;
    uint64_t last_send_try;
};

struct GC_Connection {
    uint64_t send_message_id;   /* message_id of the next message we send to peer */

    uint16_t send_array_start;   /* send_array index of oldest item */
    struct GC_Message_Array_Entry send_array[GCC_BUFFER_SIZE];

    uint64_t received_message_id;   /* message_id of peer's last message to us */
    struct GC_Message_Array_Entry received_array[GCC_BUFFER_SIZE];

    GC_PeerAddress   addr;   /* holds peer's extended real public key and ip_port */
    uint32_t    public_key_hash;   /* hash of peer's real encryption public key */
    uint8_t     session_public_key[ENC_PUBLIC_KEY];   /* self session public key for this peer */
    uint8_t     session_secret_key[ENC_SECRET_KEY];   /* self session secret key for this peer */
    uint8_t     shared_key[CRYPTO_SHARED_KEY_SIZE];  /* made with our session sk and peer's session pk */

    int         tcp_connection_num;
    uint64_t    last_received_direct_time;   /* the last time we received a direct packet from this peer */
    uint64_t    last_tcp_relays_shared;  /* the last time we tried to send this peer our tcp relays */

    Node_format connected_tcp_relays[MAX_FRIEND_TCP_CONNECTIONS];
    int tcp_relays_index;
    bool any_tcp_connections;


    uint64_t    last_received_ping_time;
    bool        pending_sync_request;   /* true if we have sent this peer a sync request and have not received a reply*/
    bool        pending_state_sync;    /* used for group state syncing */
    bool        handshaked; /* true if we've successfully handshaked with this peer */
    uint64_t    pending_handshake;
    uint8_t     pending_handshake_type;
    bool        is_pending_handshake_response;
    bool        is_oob_handshake;
    uint8_t     oob_relay_pk[ENC_PUBLIC_KEY];
    bool        confirmed;  /* true if this peer has given us their info */
    uint32_t    friend_shared_state_version;
    uint32_t    self_sent_shared_state_version;
};

/* Return connection object for peer_number.
 * Return NULL if peer_number is invalid.
 */
GC_Connection *gcc_get_connection(const GC_Chat *chat, int peer_number);

/* Adds data of length to gconn's send_array.
 *
 * Returns 0 on success and increments gconn's send_message_id.
 * Returns -1 on failure.
 */
int gcc_add_to_send_array(const Mono_Time *mono_time, GC_Connection *gconn, const uint8_t *data,
                          uint32_t length, uint8_t packet_type);

/* Decides if message need to be put in received_array or immediately handled.
 *
 * Return 2 if message is in correct sequence and may be handled immediately.
 * Return 1 if packet is out of sequence and added to received_array.
 * Return 0 if message is a duplicate.
 * Return -1 on failure
 */
int gcc_handle_received_message(GC_Chat *chat, uint32_t peer_number, const uint8_t *data, uint32_t length,
                                uint8_t packet_type, uint64_t message_id);

/* Return array index for message_id */
uint16_t get_array_index(uint64_t message_id);

/* Removes send_array item with message_id.
 *
 * Return 0 if success.
 * Return -1 on failure.
 */
int gcc_handle_ack(GC_Connection *gconn, uint64_t message_id);

bool gcc_is_ip_set(GC_Connection *gconn);

/* Checks for and handles messages that are in proper sequence in gconn's received_array.
 * This should always be called after a new packet is successfully handled.
 *
 * Return 0 on success.
 * Return -1 on failure.
 */
int gcc_check_received_array(struct Messenger *m, int group_number, uint32_t peer_number);

void gcc_resend_packets(struct Messenger *m, GC_Chat *chat, uint32_t peer_number);

/* Return true if we have a direct connection with this group connection */
bool gcc_connection_is_direct(const Mono_Time *mono_time, const GC_Connection *gconn);

/* Sends a packet to the peer associated with gconn.
 *
 * Return 0 on success.
 * Return -1 on failure.
 */
int gcc_send_group_packet(const GC_Chat *chat, const GC_Connection *gconn, const uint8_t *packet,
                          uint16_t length, uint8_t packet_type);

/* called when a peer leaves the group */
void gcc_peer_cleanup(GC_Connection *gconn);

/* called on group exit */
void gcc_cleanup(GC_Chat *chat);

#endif  /* GROUP_CONNECTION_H */

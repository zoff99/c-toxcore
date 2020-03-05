/* group_chats.h
 *
 * An implementation of massive text only group chats.
 *
 *
 *  Copyright (C) 2015 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef GROUP_CHATS_H
#define GROUP_CHATS_H

#include <stdbool.h>
#include "TCP_connection.h"
#include "group_announce.h"

#define TIME_STAMP_SIZE (sizeof(uint64_t))
#define HASH_ID_BYTES (sizeof(uint32_t))

#define MAX_GC_NICK_SIZE 128
#define MAX_GC_TOPIC_SIZE 512
#define MAX_GC_GROUP_NAME_SIZE 48
#define MAX_GC_MESSAGE_SIZE 1372
#define MAX_GC_PART_MESSAGE_SIZE 128
#define MAX_GC_PEER_ADDRS 30
#define MAX_GC_PASSWORD_SIZE 32
#define MAX_GC_MODERATORS 128
#define MAX_GC_SAVED_INVITES 50

#define GC_MOD_LIST_ENTRY_SIZE SIG_PUBLIC_KEY
#define GC_MODERATION_HASH_SIZE CRYPTO_SHA256_SIZE
#define GC_PING_INTERVAL 12
#define GC_CONFIRMED_PEER_TIMEOUT (GC_PING_INTERVAL * 4 + 10)
#define GC_UNCONFIRMED_PEER_TIMEOUT (GC_PING_INTERVAL * 2)
#define MAX_GC_CONFIRMED_PEERS 20

#define GC_JOIN_DATA_LENGTH (ENC_PUBLIC_KEY + CHAT_ID_SIZE)


typedef enum GROUP_PRIVACY_STATE {
    GI_PUBLIC,
    GI_PRIVATE,
    GI_INVALID
} GROUP_PRIVACY_STATE;

typedef enum GROUP_MODERATION_EVENT {
    MV_KICK,
    MV_BAN,
    MV_OBSERVER,
    MV_USER,
    MV_MODERATOR,
    MV_INVALID
} GROUP_MODERATION_EVENT;

typedef enum GROUP_INVITE_MESSAGE_TYPE {
    GROUP_INVITE,
    GROUP_INVITE_ACCEPTED,
    GROUP_INVITE_CONFIRMATION
} GROUP_INVITE_MESSAGE_TYPE;

/* Group roles are hierarchical where each role has a set of privileges plus
 * all the privileges of the roles below it.
 *
 * - FOUNDER is all-powerful. Cannot be demoted or banned.
 * - OP may issue bans, promotions and demotions to all roles below founder.
 * - USER may talk, stream A/V, and change the group topic.
 * - OBSERVER cannot interact with the group but may observe.
 */
typedef enum GROUP_ROLE {
    GR_FOUNDER,
    GR_MODERATOR,
    GR_USER,
    GR_OBSERVER,
    GR_INVALID
} GROUP_ROLE;

typedef enum GROUP_PEER_STATUS {
    GS_NONE,
    GS_AWAY,
    GS_BUSY,
    GS_INVALID
} GROUP_PEER_STATUS;

typedef enum GROUP_CONNECTION_STATE {
    CS_NONE,
    CS_FAILED,
    CS_DISCONNECTED,
    CS_MANUALLY_DISCONNECTED,
    CS_CONNECTING,
    CS_CONNECTED,
    CS_INVALID
} GROUP_CONNECTION_STATE;

typedef enum SAVED_GROUP_CONNECTION_STATE {
    SGCS_DISCONNECTED,
    SGCS_CONNECTED
} SAVED_GROUP_CONNECTION_STATE;

typedef enum GROUP_JOIN_REJECTED {
    GJ_NICK_TAKEN,
    GJ_NICK_BANNED,
    GJ_GROUP_FULL,
    GJ_INVALID_PASSWORD,
    GJ_INVITE_FAILED,
    GJ_INVALID
} GROUP_JOIN_REJECTED;

typedef enum GROUP_BROADCAST_TYPE {
    GM_STATUS,
    GM_NICK,
    GM_PLAIN_MESSAGE,
    GM_ACTION_MESSAGE,
    GM_PRIVATE_MESSAGE,
    GM_PEER_EXIT,
    GM_REMOVE_PEER,
    GM_REMOVE_BAN,
    GM_SET_MOD,
    GM_SET_OBSERVER,
} GROUP_BROADCAST_TYPE;

typedef enum GROUP_PACKET_TYPE {
    /* lossy packets (ID 0 is reserved) */
    GP_PING                     = 1,
    GP_MESSAGE_ACK              = 2,
    GP_INVITE_RESPONSE_REJECT   = 3,
    GP_TCP_RELAYS               = 4,

    /* lossless packets */
    GP_CUSTOM_PACKET            = 241,
    GP_PEER_ANNOUNCE            = 242,
    GP_BROADCAST                = 243,
    GP_PEER_INFO_REQUEST        = 244,
    GP_PEER_INFO_RESPONSE       = 245,
    GP_INVITE_REQUEST           = 246,
    GP_INVITE_RESPONSE          = 247,
    GP_SYNC_REQUEST             = 248,
    GP_SYNC_RESPONSE            = 249,
    GP_TOPIC                    = 250,
    GP_SHARED_STATE             = 251,
    GP_MOD_LIST                 = 252,
    GP_SANCTIONS_LIST           = 253,
    GP_FRIEND_INVITE            = 254,
    GP_HS_RESPONSE_ACK          = 255
} GROUP_PACKET_TYPE;

typedef enum GROUP_HANDSHAKE_JOIN_TYPE {
    HJ_PUBLIC,
    HJ_PRIVATE
} GROUP_HANDSHAKE_JOIN_TYPE;

typedef enum GROUP_MESSAGE_TYPE {
    GC_MESSAGE_TYPE_NORMAL,
    GC_MESSAGE_TYPE_ACTION,
} GROUP_MESSAGE_TYPE;

struct GC_Sanction_Creds {
    uint32_t    version;
    uint8_t     hash[GC_MODERATION_HASH_SIZE];    /* hash of all sanctions list signatures + version */
    uint8_t     sig_pk[SIG_PUBLIC_KEY];    /* Last mod to have modified the sanctions list*/
    uint8_t     sig[SIGNATURE_SIZE];    /* signature of hash, signed by sig_pk */
};

typedef struct GC_Moderation {
    struct GC_Sanction *sanctions;
    struct GC_Sanction_Creds sanctions_creds;
    uint32_t    num_sanctions;

    uint8_t     **mod_list;    /* Array of public signature keys of all the mods */
    uint16_t    num_mods;
} GC_Moderation;

typedef struct GC_PeerAddress {
    uint8_t     public_key[EXT_PUBLIC_KEY];
    IP_Port     ip_port;
} GC_PeerAddress;

typedef struct GC_SavedPeerInfo {
    uint8_t     public_key[EXT_PUBLIC_KEY];
    Node_format tcp_relay;
    IP_Port     ip_port;
} GC_SavedPeerInfo;

typedef struct GC_SelfPeerInfo {
    uint8_t nick[MAX_GC_NICK_SIZE];
    uint16_t nick_length;
    GROUP_PEER_STATUS user_status;
} GC_SelfPeerInfo;

typedef struct {
    uint8_t     role;
    uint8_t     nick[MAX_GC_NICK_SIZE];
    uint16_t    nick_length;
    uint8_t     status;

    /* Below variables are not sent to other peers */
    uint32_t    peer_id;    /* Permanent ID (used for the public API) */
    bool        ignore;
} GC_GroupPeer;

typedef struct {
    uint8_t     founder_public_key[EXT_PUBLIC_KEY];
    uint32_t    maxpeers;
    uint16_t    group_name_len;
    uint8_t     group_name[MAX_GC_GROUP_NAME_SIZE];
    uint8_t     privacy_state;   /* GI_PUBLIC (uses DHT) or GI_PRIVATE (invite only) */
    uint16_t    password_length;
    uint8_t     password[MAX_GC_PASSWORD_SIZE];
    uint8_t     mod_list_hash[GC_MODERATION_HASH_SIZE];
    uint32_t    version;
} GC_SharedState;

typedef struct {
    uint8_t     topic[MAX_GC_TOPIC_SIZE];
    uint16_t    length;
    uint8_t     public_sig_key[SIG_PUBLIC_KEY];   /* Public signature key of the topic setter */
    uint32_t    version;
} GC_TopicInfo;

typedef struct GC_Connection GC_Connection;


#define GROUP_SAVE_MAX_PEERS MAX_GC_PEER_ADDRS

struct Saved_Group {
    /* Group shared state */
    uint8_t   founder_public_key[EXT_PUBLIC_KEY];
    uint16_t  maxpeers;
    uint16_t  group_name_length;
    uint8_t   group_name[MAX_GC_GROUP_NAME_SIZE];
    uint8_t   privacy_state;
    uint16_t  password_length;
    uint8_t   password[MAX_GC_PASSWORD_SIZE];
    uint8_t   mod_list_hash[GC_MODERATION_HASH_SIZE];
    uint32_t  shared_state_version;
    uint8_t   shared_state_signature[SIGNATURE_SIZE];

    /* Topic info */
    uint16_t  topic_length;
    uint8_t   topic[MAX_GC_TOPIC_SIZE];
    uint8_t   topic_public_sig_key[SIG_PUBLIC_KEY];
    uint32_t  topic_version;
    uint8_t   topic_signature[SIGNATURE_SIZE];

    /* Other group info */
    uint8_t   chat_public_key[EXT_PUBLIC_KEY];
    uint8_t   chat_secret_key[EXT_SECRET_KEY];
    uint16_t  num_addrs;
    GC_SavedPeerInfo addrs[GROUP_SAVE_MAX_PEERS];
    uint16_t  num_mods;
    uint8_t   mod_list[GC_MOD_LIST_ENTRY_SIZE * MAX_GC_MODERATORS];
    uint8_t   group_connection_state;

    /* self info */
    uint8_t   self_public_key[EXT_PUBLIC_KEY];
    uint8_t   self_secret_key[EXT_SECRET_KEY];
    uint8_t   self_nick[MAX_GC_NICK_SIZE];
    uint16_t  self_nick_length;
    uint8_t   self_role;
    uint8_t   self_status;
};

typedef struct Saved_Group Saved_Group;

typedef struct GC_Chat {
    const Mono_Time *mono_time;
    uint8_t confirmed_peers[MAX_GC_CONFIRMED_PEERS][ENC_PUBLIC_KEY];
    uint8_t confirmed_peers_index;
    Node_format announced_node;

    Networking_Core *net;
    TCP_Connections *tcp_conn;

    GC_GroupPeer    *group;
    GC_Connection   *gcc;
    GC_Moderation   moderation;

    GC_SharedState  shared_state;
    uint8_t         shared_state_sig[SIGNATURE_SIZE];    /* Signed by founder using the chat secret key */

    GC_TopicInfo    topic_info;
    uint8_t         topic_sig[SIGNATURE_SIZE];    /* Signed by a moderator or the founder */

    uint32_t    numpeers;
    int         group_number;

    uint8_t     chat_public_key[EXT_PUBLIC_KEY];    /* the chat_id is the sig portion */
    uint8_t     chat_secret_key[EXT_SECRET_KEY];    /* only used by the founder */
    uint32_t    chat_id_hash;    /* 32-bit hash of the chat_id */

    uint8_t     self_public_key[EXT_PUBLIC_KEY];
    uint8_t     self_secret_key[EXT_SECRET_KEY];
    uint32_t    self_public_key_hash;


    uint8_t     connection_state;
    uint64_t    last_join_attempt;
    uint64_t    last_sent_ping_time;
    uint8_t     join_type;   /* How we joined the group (invite or DHT) */

    /* keeps track of frequency of new inbound connections */
    uint8_t     connection_O_metre;
    uint64_t    connection_cooldown_timer;
    bool        block_handshakes;  // TODO: ???

    int32_t saved_invites[MAX_GC_SAVED_INVITES];
    uint8_t saved_invites_index;

    uint8_t onion_friend_public_key[ENC_PUBLIC_KEY];
    bool should_update_self_announces;
    bool should_start_sending_handshakes;

    Saved_Group *save;
} GC_Chat;

typedef struct GC_Session {
    struct Messenger          *messenger;
    GC_Chat                   *chats;
    struct GC_Announces_List  *announces_list;

    uint32_t     num_chats;

    void (*message)(struct Messenger *m, uint32_t, uint32_t, unsigned int, const uint8_t *, size_t, void *);
    void *message_userdata;
    void (*private_message)(struct Messenger *m, uint32_t, uint32_t, unsigned int, const uint8_t *, size_t, void *);
    void *private_message_userdata;
    void (*custom_packet)(struct Messenger *m, uint32_t, uint32_t, const uint8_t *, size_t, void *);
    void *custom_packet_userdata;
    void (*moderation)(struct Messenger *m, uint32_t, uint32_t, uint32_t, unsigned int, void *);
    void *moderation_userdata;
    void (*nick_change)(struct Messenger *m, uint32_t, uint32_t, const uint8_t *, size_t, void *);
    void *nick_change_userdata;
    void (*status_change)(struct Messenger *m, uint32_t, uint32_t, unsigned int, void *);
    void *status_change_userdata;
    void (*topic_change)(struct Messenger *m, uint32_t, uint32_t, const uint8_t *,  size_t, void *);
    void *topic_change_userdata;
    void (*peer_limit)(struct Messenger *m, uint32_t, uint32_t, void *);
    void *peer_limit_userdata;
    void (*privacy_state)(struct Messenger *m, uint32_t, unsigned int, void *);
    void *privacy_state_userdata;
    void (*password)(struct Messenger *m, uint32_t, const uint8_t *, size_t, void *);
    void *password_userdata;
    void (*peer_join)(struct Messenger *m, uint32_t, uint32_t, void *);
    void *peer_join_userdata;
    void (*peer_exit)(struct Messenger *m, uint32_t, uint32_t, const uint8_t *, size_t, void *);
    void *peer_exit_userdata;
    void (*self_join)(struct Messenger *m, uint32_t, void *);
    void *self_join_userdata;
    void (*rejected)(struct Messenger *m, uint32_t, unsigned int, void *);
    void *rejected_userdata;
} GC_Session;

void pack_group_info(const GC_Chat *chat, Saved_Group *temp, bool can_use_cached_value);

bool is_public_chat(const GC_Chat *chat);

/* Sends a plain message or an action, depending on type.
 *
 * Returns 0 on success.
 * Returns -1 if the message is too long.
 * Returns -2 if the message pointer is NULL or length is zero.
 * Returns -3 if the message type is invalid.
 * Returns -4 if the sender has the observer role.
 * Returns -5 if the packet fails to send.
 */
int gc_send_message(GC_Chat *chat, const uint8_t *message, uint16_t length, uint8_t type);

/* Sends a private message to peer_id.
 *
 * Returns 0 on success.
 * Returns -1 if the message is too long.
 * Returns -2 if the message pointer is NULL or length is zero.
 * Returns -3 if the peer_id is invalid.
 * Returns -4 if the sender has the observer role.
 * Returns -5 if the packet fails to send.
 */
int gc_send_private_message(GC_Chat *chat, uint32_t peer_id, uint8_t type, const uint8_t *message, uint16_t length);

/* Sends a custom packet to the group. If lossless is true, the packet will be lossless.
 *
 * Returns 0 on success.
 * Returns -1 if the message is too long.
 * Returns -2 if the message pointer is NULL or length is zero.
 * Returns -3 if the sender has the observer role.
 */
int gc_send_custom_packet(GC_Chat *chat, bool lossless, const uint8_t *data, uint32_t length);

/* Toggles ignore for peer_id.
 *
 * Returns 0 on success.
 * Returns -1 if the peer_id is invalid.
 */
int gc_toggle_ignore(GC_Chat *chat, uint32_t peer_id, bool ignore);

/* Sets the group topic and broadcasts it to the group.
 *
 * Returns 0 on success. Setter must be a moderator or founder.
 * Returns -1 if the topic is too long.
 * Returns -2 if the caller does not have the required permissions to set the topic.
 * Returns -3 if the packet cannot be created or signing fails.
 * Returns -4 if the packet fails
 */
int gc_set_topic(GC_Chat *chat, const uint8_t *topic, uint16_t length);

/* Copies the group topic to topic. */
void gc_get_topic(const GC_Chat *chat, uint8_t *topic);

/* Returns topic length. */
uint16_t gc_get_topic_size(const GC_Chat *chat);

/* Copies group name to groupname. */
void gc_get_group_name(const GC_Chat *chat, uint8_t *group_name);

/* Returns group name length */
uint16_t gc_get_group_name_size(const GC_Chat *chat);

/* Copies the group password to password */
void gc_get_password(const GC_Chat *chat, uint8_t *password);

/* Returns the group password length */
uint16_t gc_get_password_size(const GC_Chat *chat);

/* Returns group privacy state */
uint8_t gc_get_privacy_state(const GC_Chat *chat);

/* Returns the group peer limit. */
uint32_t gc_get_max_peers(const GC_Chat *chat);

/*
 * Sets your own nick.
 *
 * Returns 0 on success.
 * Returns -1 if group_number is invalid.
 * Returns -2 if the length is too long.
 * Returns -3 if the length is zero or nick is a NULL pointer.
 * Returns -4 if the nick is already taken.
 * Returns -5 if the packet fails to send.
 */
int gc_set_self_nick(struct Messenger *m, int group_number, const uint8_t *nick, uint16_t length);

/* Copies your own nick to nick */
void gc_get_self_nick(const GC_Chat *chat, uint8_t *nick);

/* Return your own nick length */
uint16_t gc_get_self_nick_size(const GC_Chat *chat);

/* Return your own group role */
uint8_t gc_get_self_role(const GC_Chat *chat);

/* Return your own status */
uint8_t gc_get_self_status(const GC_Chat *chat);

/* Returns your own peer id */
uint32_t gc_get_self_peer_id(const GC_Chat *chat);

/* Copies your own public key to public_key */
void gc_get_self_public_key(const GC_Chat *chat, uint8_t *public_key);

/* Copies peer_id's nick to name.
 *
 * Returns 0 on success.
 * Returns -1 if peer_id is invalid.
 */
int gc_get_peer_nick(const GC_Chat *chat, uint32_t peer_id, uint8_t *name);

/* Returns peer_id's nick length.
 * Returns -1 if peer_id is invalid.
 */
int gc_get_peer_nick_size(const GC_Chat *chat, uint32_t peer_id);

/* Copies peer_id's public key to public_key.
 *
 * Returns 0 on success.
 * Returns -1 if peer_id is invalid.
 */
int gc_get_peer_public_key(const GC_Chat *chat, uint32_t peer_id, uint8_t *public_key);

int gc_get_peer_public_key_by_peer_id(const GC_Chat *chat, uint32_t peer_id, uint8_t *public_key);

/* Sets the caller's status to status
 *
 * Returns 0 on success.
 * Returns -1 if the group_number is invalid.
 * Returns -2 if the status type is invalid.
 * Returns -3 if the packet failed to send.
 */
int gc_set_self_status(struct Messenger *m, int group_number, uint8_t status);

/* Returns peer_id's status.
 * Returns (uint8_t) -1 on failure.
 */
uint8_t gc_get_status(const GC_Chat *chat, uint32_t peer_id);

/* Returns peer_id's group role.
 * Returns (uint8_t) -1 on failure.
 */
uint8_t gc_get_role(const GC_Chat *chat, uint32_t peer_id);

/* Sets the role of peer_id. role must be one of: GR_MODERATOR, GR_USER, GR_OBSERVER
 *
 * Returns 0 on success.
 * Returns -1 if the group_number is invalid.
 * Returns -2 if the peer_id is invalid.
 * Returns -3 if caller does not have sufficient permissions for the action.
 * Returns -4 if the role assignment is invalid.
 * Returns -5 if the role failed to be set.
 */
int gc_set_peer_role(struct Messenger *m, int group_number, uint32_t peer_id, uint8_t role);

/* Sets the group password and distributes the new shared state to the group.
 *
 * This function requires that the shared state be re-signed and will only work for the group founder.
 *
 * Returns 0 on success.
 * Returns -1 if the caller does not have sufficient permissions for the action.
 * Returns -2 if the password is too long.
 * Returns -3 if the packet failed to send.
 */
int gc_founder_set_password(GC_Chat *chat, const uint8_t *password, uint16_t password_length);

/* Sets the group privacy state and distributes the new shared state to the group.
 *
 * This function requires that the shared state be re-signed and will only work for the group founder.
 *
 * Returns 0 on success.
 * Returns -1 if group_number is invalid.
 * Returns -2 if the privacy state is an invalid type.
 * Returns -3 if the caller does not have sufficient permissions for this action.
 * Returns -4 if the privacy state fails to set.
 * Returns -5 if the packet fails to send.
 */
int gc_founder_set_privacy_state(struct Messenger *m, int group_number, uint8_t new_privacy_state);

/* Sets the peer limit to maxpeers and distributes the new shared state to the group.
 *
 * This function requires that the shared state be re-signed and will only work for the group founder.
 *
 * Returns 0 on success.
 * Returns -1 if the caller does not have sufficient permissions for this action.
 * Returns -2 if the peer limit could not be set.
 * Returns -3 if the packet failed to send.
 */
int gc_founder_set_max_peers(GC_Chat *chat, uint32_t max_peers);

/* Instructs all peers to remove peer_id from their peerlist.
 * If set_ban is true peer will be added to the ban list.
 *
 * Returns 0 on success.
 * Returns -1 if the group_number is invalid.
 * Returns -2 if the peer_id is invalid.
 * Returns -3 if the caller does not have sufficient permissions for this action.
 * Returns -4 if the action failed.
 * Returns -5 if the packet failed to send.
 */
int gc_remove_peer(struct Messenger *m, int group_number, uint32_t peer_id, bool set_ban, uint8_t ban_type);

/* Instructs all peers to remove ban_id from their ban list.
 *
 * Returns 0 on success.
 * Returns -1 if the caller does not have sufficient permissions for this action.
 * Returns -2 if the entry could not be removed.
 * Returns -3 if the packet failed to send.
 */
int gc_remove_ban(GC_Chat *chat, uint32_t ban_id);

/* Copies the chat_id to dest */
void gc_get_chat_id(const GC_Chat *chat, uint8_t *dest);



void gc_callback_message(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t, unsigned int,
                         const uint8_t *, size_t, void *), void *userdata);

void gc_callback_private_message(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                                 unsigned int, const uint8_t *,
                                 size_t, void *), void *userdata);

void gc_callback_custom_packet(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                               const uint8_t *, size_t, void *), void *userdata);

void gc_callback_moderation(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t, uint32_t,
                            unsigned int,
                            void *), void *userdata);

void gc_callback_nick_change(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                             const uint8_t *,
                             size_t, void *), void *userdata);

void gc_callback_status_change(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                               unsigned int, void *),
                               void *userdata);

void gc_callback_topic_change(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                              const uint8_t *,
                              size_t, void *), void *userdata);

void gc_callback_peer_limit(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t, void *),
                            void *userdata);

void gc_callback_privacy_state(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, unsigned int,
                               void *), void *userdata);

void gc_callback_password(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, const uint8_t *, size_t,
                          void *),
                          void *userdata);

void gc_callback_peer_join(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t, void *),
                           void *userdata);

void gc_callback_peer_exit(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, uint32_t,
                           const uint8_t *, size_t,
                           void *), void *userdata);

void gc_callback_self_join(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, void *),
                           void *userdata);

void gc_callback_rejected(struct Messenger *m, void (*function)(struct Messenger *m, uint32_t, unsigned int type,
                          void *),
                          void *userdata);

/* The main loop. */
void do_gc(GC_Session *c, void *userdata);

/* Returns a NULL pointer if fail.
 * Make sure that DHT is initialized before calling this
 */
GC_Session *new_dht_groupchats(struct Messenger *m);

/* Cleans up groupchat structures and calls gc_group_exit() for every group chat */
void kill_dht_groupchats(GC_Session *c);

/* Loads a previously saved group and attempts to join it.
 *
 * Returns group_number on success.
 * Returns -1 on failure.
 */
int gc_group_load(GC_Session *c, Saved_Group *save, int group_number);

/* Creates a new group.
 *
 * Return group_number on success.
 * Return -1 if the group name is too long.
 * Return -2 if the group name is empty.
 * Return -3 if the privacy state is an invalid type.
 * Return -4 if the the group object fails to initialize.
 * Return -5 if the group state fails to initialize.
 * Return -6 if the group fails to announce to the DHT.
 */
int gc_group_add(GC_Session *c, uint8_t privacy_state, const uint8_t *group_name, uint16_t group_name_length,
                 const GC_SelfPeerInfo *peer_info);

/* Sends an invite request to a public group using the chat_id.
 *
 * If the group is not password protected password should be set to NULL and password_length should be 0.
 *
 * Return group_number on success.
 * Reutrn -1 if the group object fails to initialize.
 * Return -2 if chat_id is NULL or a group with chat_id already exists in the chats arr
 * Return -3 if there is an error setting the group password.
 */
int gc_group_join(GC_Session *c, const uint8_t *chat_id, const uint8_t *passwd, uint16_t passwd_len,
                  const GC_SelfPeerInfo *peer_info);

bool gc_disconnect_from_group(GC_Session *c, GC_Chat *chat);

/* Resets chat saving all self state and attempts to reconnect to group */
bool gc_rejoin_group(GC_Session *c, GC_Chat *chat);

/* Joins a group using the invite data received in a friend's group invite.
 *
 * Return group_number on success.
 * Return -1 if the invite data is malformed.
 * Return -2 if the group object fails to initialize.
 * Return -3 if there is an error setting the password.
 */
int gc_accept_invite(GC_Session *c, int32_t friend_number, const uint8_t *data, uint16_t length,
                     const uint8_t *passwd, uint16_t passwd_len,
                     const GC_SelfPeerInfo *peer_info);

/* Invites friendnumber to chat. Packet includes: Type, chat_id, node
 *
 * Return 0 on success.
 * Return -1 if friendnumber does not exist.
 * Return -2 on failure to create the invite data.
 * Return -3 if the packet fails to send.
 */
int gc_invite_friend(GC_Session *c, GC_Chat *chat, int32_t friendnum,
                     int send_group_invite_packet(const struct Messenger *m, uint32_t friendnumber, const uint8_t *packet, size_t length));

/* Sends parting message to group and deletes group.
 *
 * Return 0 on success.
 * Return -1 if the parting message is too long.
 * Return -2 if the parting message failed to send.
 * Return -3 if the group instance failed delete.
 */
int gc_group_exit(GC_Session *c, GC_Chat *chat, const uint8_t *message, uint16_t length);

/* Count number of active groups.
 *
 * Returns the count.
 */
uint32_t gc_count_groups(const GC_Session *c);

void gc_copy_groups_numbers(const GC_Session *c, uint32_t *list);

/* Returns true if peer_number exists */
bool peer_number_valid(const GC_Chat *chat, int peer_number);

/* Return group_number's GC_Chat pointer on success
 * Return NULL on failure
 */
GC_Chat *gc_get_group(const GC_Session *c, int group_number);

/* Deletes peer_number from group.
 *
 * Return 0 on success.
 * Return -1 on failure.
 */
int gc_peer_delete(struct Messenger *m, int group_number, uint32_t peer_number, const uint8_t *data, uint16_t length);

/* Copies up to max_addrs peer addresses from chat into addrs.
 *
 * Returns number of addresses copied.
 */
uint16_t gc_copy_peer_addrs(const GC_Chat *chat, GC_SavedPeerInfo *addrs, size_t max_addrs);

/* If read_id is non-zero sends a read-receipt for ack_id's packet.
 * If request_id is non-zero sends a request for the respective id's packet.
 */
int gc_send_message_ack(const GC_Chat *chat, GC_Connection *gconn, uint64_t read_id, uint64_t request_id);

int handle_gc_lossless_helper(struct Messenger *m, int group_number, uint32_t peer_number, const uint8_t *data,
                              uint16_t length, uint64_t message_id, uint8_t packet_type);

/* Sends the sanctions list to all peers in group.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
int broadcast_gc_sanctions_list(GC_Chat *chat);


int handle_gc_invite_accepted_packet(GC_Session *c, int friend_number, const uint8_t *data,
                                     uint32_t length);

bool check_group_invite(GC_Session *c, const uint8_t *data, uint32_t length);

int handle_gc_invite_confirmed_packet(GC_Session *c, int friend_number, const uint8_t *data,
                                      uint32_t length);

GC_Chat *gc_get_group_by_public_key(const GC_Session *c, const uint8_t *public_key);

int add_peers_from_announces(const GC_Session *gc_session, GC_Chat *chat, GC_Announce *announces, uint8_t gc_announces_count);


size_t group_get_peers_list_size(const GC_Chat *chat);

void group_get_peers_list(const GC_Chat *chat, uint32_t *peers_list);

#endif  /* GROUP_CHATS_H */

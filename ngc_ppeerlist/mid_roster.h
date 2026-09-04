/*
mid_roster.h
Minimal persistent group-roster middleware for Tox NGC.
This header exposes a very simple API. All internal state, heartbeats,
online-state synchronization, and roster requests are hidden inside
the middleware. The client only needs to forward Toxcore events to
the middleware hooks and call mid_iterate() in its main loop.
*/

#ifndef MID_ROSTER_H
#define MID_ROSTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

/*
Opaque middleware state.
A single MidState instance can manage multiple groups simultaneously.

THREAD-SAFETY:
All public functions in this header are thread-safe and may be called
concurrently from any thread. Internally a single non-recursive
pthread_mutex_t protects all mutable state (similar to tox_lock/tox_unlock
in Toxcore). Callbacks are always fired WITHOUT the lock held, so it is
safe to call any mid_* function from within a callback without deadlock.

mid_new() and mid_free() are NOT thread-safe with respect to each other
or to concurrent users. The caller must ensure mid_free() is only called
after all other threads have stopped using the MidState.
*/
typedef struct MidState MidState;

/* Hard limit for stored peers per group. */
#define MID_MAX_PEERS_PER_GROUP 1024

#define MID_PROTOCOL_VERSION    1

/* NGC custom packet ID */
#define MID_MAGIC_0  0x66      // was before: 0xA0
#define MID_MAGIC_1  0x77      // was before: 0x91
#define MID_MAGIC_2  0x92

#define MID_MAGIC_BYTES_TOTAL 3
/* Total header size: Magic bytes + Protocol Version (1 byte) + Message Type (1 byte) */
#define MID_HEADER_SIZE (MID_MAGIC_BYTES_TOTAL + 2)

/******************************************************************************
Key-size constants (needed by MidPeerInfo below)
******************************************************************************/

#ifndef MID_IDENTITY_KEY_SIZE
#define MID_IDENTITY_KEY_SIZE TOX_GROUP_PEER_PUBLIC_KEY_SIZE
#endif

#ifndef MID_SIGNING_KEY_SIZE
#define MID_SIGNING_KEY_SIZE 32
#endif

#ifndef MID_MAX_NICK_SIZE
#define MID_MAX_NICK_SIZE 128
#endif

/******************************************************************************
Lifecycle
******************************************************************************/

/*
Create a new middleware state.

Parameters:
  save_path      File path to load/save state. Pass NULL for a clean,
                 in-memory only state.

  passphrase     Optional passphrase to encrypt/decrypt the save file on disk.
                 Pass NULL (with passphrase_len = 0) to save in plaintext
                 (file permissions will be set to 0600).
                 The passphrase can be any arbitrary length. It is fed
                 directly to tox_pass_key_derive() internally.
                 The passphrase is NOT stored in the middleware state.
                 You must provide the same passphrase on every call to
                 mid_save() to produce a readable encrypted file.

  passphrase_len Length of the passphrase in bytes. Pass 0 if passphrase
                 is NULL.

Behavior on load:
  If a save file exists at save_path:
    - If the file is encrypted, the passphrase MUST be correct, otherwise
      decryption fails and the file is ignored (clean state is started).
    - If the file is unencrypted, it is loaded regardless of passphrase.
    - If the file is corrupted or has an unsupported version, it is ignored.

Returns:
  A new MidState instance, or NULL on fatal error.
*/
MidState *mid_new(const char *save_path, const uint8_t *passphrase, size_t passphrase_len);

/*
Save the current state to disk. Thread-safe.

The state is written atomically: data is written to a temporary file first,
then renamed over the target path. This prevents corruption if the process
is killed mid-write.

Parameters:
  s              The middleware state. Must not be NULL.

  passphrase     Optional passphrase to encrypt the save file on disk.
                 Pass NULL (with passphrase_len = 0) to save in plaintext.
                 Must match the passphrase used when the file was originally
                 encrypted if you intend to load it later with a passphrase.
                 The passphrase is NOT stored. It is used only for this
                 single save operation.

  passphrase_len Length of the passphrase in bytes. Pass 0 if passphrase
                 is NULL.

Note:
  The save_path used here is the one provided to mid_new(). There is no
  separate configuration function. If save_path was NULL at creation,
  this function returns false immediately.

Returns:
  true on success, false on failure.
*/
bool mid_save(MidState *s, const uint8_t *passphrase, size_t passphrase_len);

void mid_free(MidState *s);

/******************************************************************************
Group Hooks
******************************************************************************/

/*
Call this when the client successfully joins a group (or creates one).
The middleware will fetch keys, announce the client, and request the roster.
*/
void mid_on_group_self_join(MidState *s, Tox *tox, uint32_t group_number, const uint8_t *nickname, size_t nickname_len);

/*
Call this when the client leaves or deletes a group.
The middleware will securely wipe keys and free the roster for this group.
*/
void mid_on_group_delete(MidState *s, Tox *tox, uint32_t group_number);

/******************************************************************************
Peer Event Hooks
******************************************************************************/

/*
Call this when a new peer joins the group.
The middleware will record them as online, refresh their name, and
automatically re-sync the roster if we have already announced ourselves.
*/
void mid_on_group_peer_join(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id);

/*
 * Call this when a peer exits the group (via Toxcore's peer_exit callback).
 * 
 * NOTE: The `exit_type` parameter is intentionally ignored. 
 * To maintain a stable, persistent roster that includes offline peers, 
 * we do not delete peers simply because they disconnected or quit. 
 * 
 * Instead, this triggers a state-sync (`mid_sync_online_state_group`). 
 * The middleware will query Toxcore to confirm the peer is gone and mark 
 * them as offline (`TOX_CONNECTION_NONE`). 
 * 
 * If a peer actually intends to leave permanently, they should broadcast 
 * a signed LEFT tombstone (MID_STATUS_LEFT) via custom packets before 
 * disconnecting. The middleware will process that tombstone and keep the 
 * peer in the roster for 7 days (MID_TOMBSTONE_TTL_SEC) to allow offline 
 * peers to sync the departure.
 */
void mid_on_group_peer_exit(MidState *s, Tox *tox, uint32_t group_number, Tox_Group_Exit_Type exit_type);

/*
Call this when a peer changes their nickname.
The middleware will update the unsigned local observation of their name.
*/
void mid_on_group_peer_name(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id);

/******************************************************************************
Moderation Hook
******************************************************************************/

/*
Call this when a group moderation event is received.
The middleware hides all kick-related roster logic internally:
If we were the target of the kick, the middleware securely wipes the group
state (keys and roster) and returns true. The client should then clear its
own local group reference (group_number / group_connected).
If another peer was kicked, the middleware deletes that peer from the
roster and returns false.

NOTE:
The peer who INITIATES a kick does NOT receive the group_moderation event
(see tox_group_mod_kick_peer / tox_callback_group_moderation in tox.h).
Therefore the middleware cannot delete the kicked peer on the kicker's side
from this hook. The kicker must explicitly call mid_delete_peer_by_identity()
for the peer it kicked.

Returns true if we ourselves were kicked.
*/
bool mid_on_group_moderation(MidState *s, Tox *tox, uint32_t group_number,
                             uint32_t source_peer_id, uint32_t target_peer_id,
                             Tox_Group_Mod_Event mod_type);

/******************************************************************************
Network Hook
******************************************************************************/

/*
Call this when a group custom packet is received.
The middleware will verify signatures, upsert records, and respond to
roster requests automatically.
*/
void mid_on_group_custom_packet(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *data, size_t length);

/******************************************************************************
Main Loop Hook
******************************************************************************/

/*
Call this in your main loop alongside tox_iterate().
The middleware will handle periodic heartbeats and continuously sync
the online state of all peers across all managed groups.
*/
void mid_iterate(MidState *s, Tox *tox);

/******************************************************************************
Queries
******************************************************************************/

size_t mid_group_count(MidState *s);
size_t mid_peer_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE]);
size_t mid_signed_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE]);
size_t mid_online_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE]);
size_t mid_offline_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE]);

/*
Find a peer by NGC identity public key in a specific group.
Returns:
>= 0   peer index in the middleware roster for that group
-1     peer not found, or group not found
*/
int mid_find_peer(MidState *s,
                  const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                  const uint8_t identity_key[TOX_GROUP_PEER_PUBLIC_KEY_SIZE]);

/*
Delete a peer from the middleware roster by identity key.
This is needed for kick handling.
The kicker does not receive a group_peer_exit event for the peer it kicked,
so the client must explicitly delete the kicked peer.
*/
bool mid_delete_peer_by_identity(MidState *s,
                                 const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                                 const uint8_t identity_key[TOX_GROUP_PEER_PUBLIC_KEY_SIZE]);

/*
Returns true if the middleware has a signed LEFT tombstone for this
identity key in the given group.
*/
bool mid_peer_is_signed_left(MidState *s,
                             const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                             const uint8_t identity_key[MID_IDENTITY_KEY_SIZE]);

/*
Call this IMMEDIATELY BEFORE calling tox_group_leave().
This broadcasts a signed "LEFT" tombstone to the group.
When other peers (including those who were offline and just synced)
receive this signed record, they will permanently delete this peer
from their persistent rosters.
*/
bool mid_announce_leave(MidState *s, Tox *tox, uint32_t group_number);


bool mid_self_set_name(MidState *s, Tox *tox, uint32_t group_number, const uint8_t *nickname, size_t nickname_len);

/******************************************************************************
Peer List Query API (for C / JNI clients)
Flat, fixed-size, index-based access. No dynamic allocation.
Safe pattern:

    size_t n = mid_peer_list_count(s, chat_id);
    for (size_t i = 0; i < n; i++) {
        MidPeerInfo info;
        if (mid_peer_list_get(s, chat_id, i, &info)) {
            // use info
        }
    }
******************************************************************************/

/*
Flat representation of one peer in the roster.
All fields are fixed-size. No pointers. NUL-terminated nickname.
Suitable for direct mapping to JNI structs or flatbuffers.
*/
typedef struct {
    uint8_t  identity_key[MID_IDENTITY_KEY_SIZE];   /*  NGC group identity key  */
    uint8_t  signing_key[MID_SIGNING_KEY_SIZE];     /*  Ed25519 signing key    */
    uint8_t  status;          /*  0 = ACTIVE, 1 = LEFT  */
    uint8_t  connection_status; /* Tox_Connection: 0=NONE(offline), 1=TCP, 2=UDP */
    uint8_t  role;            /* Tox_Group_Role: 0=FOUNDER, 1=MOD, 2=USER, 3=OBSERVER */
    uint8_t  has_signature;   /*  1 if record is cryptographically signed  */
    uint8_t  reserved;        /*  padding for alignment  */
    uint64_t last_seen;       /*  unix timestamp, 0 if never seen  */
    uint16_t nickname_len;    /*  length of nickname in bytes (without NUL)  */
    /* CRUCIAL: +1 byte to safely hold the NUL terminator for JNI/C strings */
    uint8_t nickname[MID_MAX_NICK_SIZE + 1];
} MidPeerInfo;

/*
Return the number of peers in the middleware roster for a group.
Returns 0 if the group is not found.
*/
size_t mid_peer_list_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE]);

/*
Fill `out` with the peer at `index` in the roster for the given group.
Returns true on success.
Returns false if s is NULL, out is NULL, group not found, or index >= count.
The caller provides the MidPeerInfo storage. No memory is allocated.
All fields are written; the struct is fully initialised on success.
*/
bool mid_peer_list_get(MidState *s,
                       const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                       size_t index,
                       MidPeerInfo *out);

/******************************************************************************
Peer List Changed Callback
Register a callback to be notified whenever the peer roster for any group
managed by this MidState has changed. The client should then call
mid_peer_list_count() / mid_peer_list_get() to refresh its view.
The callback receives the chat_id whose list changed, plus the
user_data pointer.

Thread-safety: the callback is invoked WITHOUT the middleware lock held.
It is safe to call mid_peer_list_get() from within the callback.
******************************************************************************/

typedef void (*mid_peer_list_changed_cb)(const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], void *user_data);

/*
Register the peer-list-changed callback.
Pass NULL for cb to unregister.
*/
void mid_set_peer_list_changed_cb(MidState *s,
                                  mid_peer_list_changed_cb cb,
                                  void *user_data);

/******************************************************************************
Network Stats
******************************************************************************/

/*
Query the total bytes sent and received by the middleware custom packets.
Pass NULL for either pointer if you only want to query one of them.
Thread-safe.
*/
void mid_get_network_stats(MidState *s, uint64_t *sent_bytes, uint64_t *recv_bytes);

/******************************************************************************
Debug
******************************************************************************/

void mid_print_peer_table(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const char *title);

#endif /* MID_ROSTER_H */

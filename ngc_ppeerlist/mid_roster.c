/*
mid_roster.c
Minimal persistent group-roster middleware for Tox NGC.

Goal:
Maintain a stable peer list that can include offline peers.
This is a best-effort, eventually consistent P2P solution.

Transport:
Uses only tox_group_send_custom_packet().

Crypto:
Uses libsodium Ed25519 signatures.
Each peer signs their own JOIN / LEAVE / presence updates.

KEY MODEL -- VERY IMPORTANT

Tox NGC has multiple keys. This middleware uses three distinct keys:

identity_key
This is the normal 32-byte Tox NGC group peer public key.
Tox NGC exposes a 32-byte group identity public key:
tox_group_self_get_public_key()
tox_group_peer_get_public_key()
This key is used as the main roster lookup key.
It is the persistent peer identity inside the group.
In typical NGC extended-key layout, this is the Curve25519 encryption
public key derived from the peer's Ed25519 signing key.

signing_key
This is the 32-byte Ed25519 public signing key belonging to the same
NGC group identity.
For signing/verification we also need Ed25519 keys:
tox_group_self_get_signing_secret_key()      [patched]
tox_group_self_get_signing_public_key()      [patched]
For other peers we need:
tox_group_peer_get_signing_public_key()      [patched]
This key is used to verify signatures made by that peer.

self_secret_signing_key
This is the Ed25519 secret signing key for our own NGC group identity.
It is obtained from the patched API:
tox_group_self_get_signing_secret_key()
This is NOT:
the long-term Tox one-to-one secret key
the NGC encryption secret key
the group shared secret
It is specifically the Ed25519 signing secret key.

Signed presence records
A presence record contains:
status          ACTIVE / LEFT
timestamp       sender-generated timestamp / logical version
nickname        optional peer nickname
identity_key    normal Tox NGC identity public key
signing_key     Ed25519 public signing key
The record is signed with the peer's Ed25519 signing secret key.
The signature is verified with the Ed25519 signing public key.
The identity_key and signing_key are both included in the signed body so
that offline peers can be verified later as long as their signing key is
known.

Key binding:
This middleware checks that the Ed25519 signing key belongs to the Tox
NGC identity key.
In normal NGC extended-key design:
identity_key == curve25519_from_ed25519(signing_key)
For compatibility, direct equality is also accepted:
identity_key == signing_key

Storage:
No storage backend is implemented.
Use:
mid_peer_count()
mid_signed_count()
mid_online_count()
to attach any storage backend later.

Nickname safety:
Toxcore nicknames are raw uint8_t buffers. They are NOT guaranteed to be
valid UTF-8, NOT guaranteed to be printable ASCII, and NOT guaranteed to
be NUL-terminated. They may contain embedded NUL bytes or arbitrary binary.
This middleware treats all nicknames as opaque byte arrays with an
explicit length. No C-string functions (strlen, strcmp, strcpy, printf %s)
are ever used on nickname data. All copies are bounded by explicit lengths
and destination buffers are zero-filled before use.

Thread-safety model:
All public API functions acquire a single non-recursive pthread_mutex_t
(similar to tox_lock/tox_unlock in Toxcore) before touching any internal
state. Internal "_internal" helper functions assume the lock is ALREADY
held and must NEVER be called without it. Callbacks are always invoked
AFTER the lock is released to prevent deadlocks when the client calls
back into the middleware from within the callback.

IMPORTANT: Internal functions must only call other "_internal" functions,
never the public (locking) wrappers. This guarantees no deadlock with a
non-recursive mutex.

Requirements:
Toxcore NGC
tox_group_send_custom_packet()
tox_group_self_get_public_key()
tox_group_peer_get_public_key()
tox_group_self_get_signing_secret_key()      [patched]
tox_group_self_get_signing_public_key()      [patched]
tox_group_peer_get_signing_public_key()      [patched]
libsodium
*/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include "../toxcore/tox.h"
#include "../toxencryptsave/toxencryptsave.h"
#include <sodium.h>
#include "mid_roster.h"

#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef MID_DEBUG_LOGGING
#define printf(...) ((void)0)
#define fflush(x)   ((void)0)
#endif


/******************************************************************************
Tox key-size constants
******************************************************************************/

#ifndef TOX_GROUP_PEER_PUBLIC_KEY_SIZE
#define TOX_GROUP_PEER_PUBLIC_KEY_SIZE 32
#endif

#ifndef TOX_GROUP_SIGNING_PUBLIC_KEY_SIZE
#define TOX_GROUP_SIGNING_PUBLIC_KEY_SIZE 32
#endif

#ifndef TOX_GROUP_SIGNING_SECRET_KEY_SIZE
#define TOX_GROUP_SIGNING_SECRET_KEY_SIZE 64
#endif

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

/******************************************************************************
Middleware key-size constants
******************************************************************************/

static const uint8_t MID_MAGIC[MID_MAGIC_BYTES_TOTAL] = {
    MID_MAGIC_0,
    MID_MAGIC_1,
    MID_MAGIC_2
};

#define MID_SAVE_MAGIC       "MIDR"
#define MID_SAVE_MAGIC_SIZE  4
#define MID_SAVE_VERSION     3
#define MID_MAX_GROUPS       1000

#define MID_BUF_INIT_CAP 4096
#define MID_MAX_CHANGED_GROUPS_PER_ITERATE 256

#define MID_SIGNING_SECRET_KEY_SIZE TOX_GROUP_SIGNING_SECRET_KEY_SIZE

#if MID_IDENTITY_KEY_SIZE != 32
#error "mid_roster.c currently requires TOX_GROUP_PEER_PUBLIC_KEY_SIZE == 32"
#endif

#if MID_SIGNING_KEY_SIZE != 32
#error "mid_roster.c currently requires TOX_GROUP_SIGNING_PUBLIC_KEY_SIZE == 32"
#endif

#if MID_SIGNING_SECRET_KEY_SIZE != 64
#error "mid_roster.c requires TOX_GROUP_SIGNING_SECRET_KEY_SIZE == 64"
#endif

#define MID_SIG_SIZE            64
#define MID_MAX_PACKET_SIZE     1200
#define MID_ROSTER_COOLDOWN_SEC 30
/* FIX 4: Increased heartbeat interval from 50s to 300s (5 mins) to save traffic */
#define MID_HEARTBEAT_SEC       300

#define MID_RECORD_STATUS_SIZE    sizeof(uint8_t)
#define MID_RECORD_TIMESTAMP_SIZE sizeof(uint64_t)
#define MID_RECORD_NICKLEN_SIZE   sizeof(uint16_t)

#define MID_RECORD_FIXED_BODY_SIZE (MID_RECORD_STATUS_SIZE + MID_RECORD_TIMESTAMP_SIZE + MID_RECORD_NICKLEN_SIZE)

#define MID_HEARTBEAT_BODY_SIZE (MID_RECORD_STATUS_SIZE + MID_RECORD_TIMESTAMP_SIZE + MID_IDENTITY_KEY_SIZE)


/*
Throttle for the heavy sync/cleanup loop in mid_iterate().
xx second is fast enough for responsive UI, but slow enough to avoid
hammering Toxcore on every 50ms tox_iterate() tick.
*/
#define MID_SYNC_INTERVAL_SEC   5


#define MID_MAX_BODY_SIZE (MID_RECORD_FIXED_BODY_SIZE + MID_MAX_NICK_SIZE + \
                           MID_IDENTITY_KEY_SIZE + MID_SIGNING_KEY_SIZE)

/*
Time-To-Live for LEFT tombstones.
We keep them in the roster for 7 days so that offline peers who come
back online can sync and receive cryptographic proof of the departure.
*/
#define MID_TOMBSTONE_TTL_SEC (7 * 24 * 60 * 60)

/*
Time-To-Live for "Zombie" peers (offline ACTIVE peers who never sent a LEFT tombstone).
30 days is safe because Toxcore's network timeout is much shorter, and our 5-minute
heartbeats guarantee last_seen stays fresh for actually active peers.
*/
#define MID_STALE_PEER_TTL_SEC (30 * 24 * 60 * 60)


typedef enum {
    MID_STATUS_ACTIVE = 0,
    MID_STATUS_LEFT   = 1
} MidStatus;

typedef enum {
    MID_MSG_PRESENCE       = 1,
    MID_MSG_ROSTER_REQUEST = 2,
    MID_MSG_HEARTBEAT      = 3, /* FIX 3: Lightweight heartbeat message */
    MID_MSG_ROSTER_BATCH   = 4  /* FIX 5: Batched roster response message */
} MidMsg;

typedef struct {
    uint8_t identity_key[MID_IDENTITY_KEY_SIZE];
    uint8_t signing_key[MID_SIGNING_KEY_SIZE];
    uint8_t  status;
    uint64_t timestamp;
    uint8_t  nickname[MID_MAX_NICK_SIZE];
    uint16_t nickname_len;
    uint8_t  signature[MID_SIG_SIZE];
    bool     has_signature;
    Tox_Connection connection_status;
    Tox_Group_Role role;
    uint64_t last_seen;
} MidPeerRecord;

typedef struct {
    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    uint8_t self_identity_key[MID_IDENTITY_KEY_SIZE];
    uint8_t self_signing_key[MID_SIGNING_KEY_SIZE];
    uint8_t self_secret_signing_key[MID_SIGNING_SECRET_KEY_SIZE];
    bool     have_keys;
    uint8_t  self_nickname[MID_MAX_NICK_SIZE];
    uint16_t self_nickname_len;
    bool     self_active;
    bool     announced;
    MidPeerRecord *records;
    size_t         count;
    size_t         capacity;
    uint64_t last_announce;
    uint64_t last_roster_response;
    uint64_t roster_reply_deadline; /* FIX 2: Multicast suppression timer */
    uint8_t  roster_fingerprint[MID_IDENTITY_KEY_SIZE]; /* Incremental XOR sum of all signed identity keys */
} MidGroupState;

/*
The actual definition of the opaque MidState struct.
*/
struct MidState {
    MidGroupState *groups;
    size_t         group_count;
    size_t         group_capacity;

    /*
     * Peer-list-changed callback.
     * Called whenever the roster for any group is mutated.
     */
    mid_peer_list_changed_cb peer_list_changed_cb;
    void                    *peer_list_changed_user_data;

    /*
     * Thread-safety mutex (non-recursive).
     * Protects all internal state. Modeled after tox->mutex in Toxcore.
     * All public entry points call mid_lock()/mid_unlock().
     * Internal "_internal" functions assume the lock is already held.
     */
    pthread_mutex_t *mutex;

    /*
     * Timestamp of the last heavy sync/cleanup pass in mid_iterate().
     */
    uint64_t last_sync_time;

    /*
     * Network statistics (custom packets only).
     */
    uint64_t sent_bytes;
    uint64_t recv_bytes;

    char *save_path;
};

/******************************************************************************
Thread-safety helpers
Modeled after tox_lock()/tox_unlock() in Toxcore.
The mutex is non-recursive; internal code paths must never re-lock.
******************************************************************************/

static void mid_lock(MidState *s)
{
    if (s != NULL && s->mutex != NULL) {
        pthread_mutex_lock(s->mutex);
    }
}

static void mid_unlock(MidState *s)
{
    if (s != NULL && s->mutex != NULL) {
        pthread_mutex_unlock(s->mutex);
    }
}

/******************************************************************************
Small helper functions
******************************************************************************/

static uint64_t mid_now_or_time(uint64_t now)
{
    if (now != 0) {
        return now;
    }
    return (uint64_t)time(NULL);
}

static void mid_put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void mid_put_u64_be(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v & 0xFF);
}

static uint16_t mid_get_u16_be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint64_t mid_get_u64_be(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(uint64_t); i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static char *mid_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = (char *)malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

static void mid_put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v & 0xFF);
}

static uint32_t mid_get_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

/* Dynamic Buffer for Serialization */
typedef struct { uint8_t *data; size_t len; size_t cap; } MidBuf;

static void mid_buf_init(MidBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void mid_buf_free(MidBuf *b) { free(b->data); b->data = NULL; b->len = 0; b->cap = 0; }

static bool mid_buf_ensure(MidBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;

    // Prevent overflow in b->len + extra
    if (extra > SIZE_MAX - b->len) return false;
    size_t required = b->len + extra;

    size_t new_cap = (b->cap == 0) ? MID_BUF_INIT_CAP : b->cap;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) return false; // Prevent overflow
        new_cap *= 2;
    }

    uint8_t *tmp = realloc(b->data, new_cap);
    if (!tmp) return false;
    b->data = tmp; b->cap = new_cap; return true;
}

static void mid_buf_append(MidBuf *b, const void *data, size_t len) {
    if (len == 0) return;
    if (mid_buf_ensure(b, len)) { memcpy(b->data + b->len, data, len); b->len += len; }
}

static void mid_buf_append_u8(MidBuf *b, uint8_t v) { mid_buf_append(b, &v, 1); }
static void mid_buf_append_u16(MidBuf *b, uint16_t v) { uint8_t tmp[2]; mid_put_u16_be(tmp, v); mid_buf_append(b, tmp, 2); }
static void mid_buf_append_u32(MidBuf *b, uint32_t v) { uint8_t tmp[4]; mid_put_u32_be(tmp, v); mid_buf_append(b, tmp, 4); }
static void mid_buf_append_u64(MidBuf *b, uint64_t v) { uint8_t tmp[8]; mid_put_u64_be(tmp, v); mid_buf_append(b, tmp, 8); }

/* Binary Reader for Deserialization */
typedef struct { const uint8_t *data; size_t len; size_t pos; } MidReader;

static bool mid_reader_has(MidReader *r, size_t need) { return r->pos + need <= r->len; }

static bool mid_reader_read(MidReader *r, void *out, size_t len) {
    if (!mid_reader_has(r, len)) return false;
    memcpy(out, r->data + r->pos, len); r->pos += len; return true;
}

static bool mid_reader_read_u8(MidReader *r, uint8_t *out) { return mid_reader_read(r, out, 1); }

static bool mid_reader_read_u16(MidReader *r, uint16_t *out) {
    uint8_t tmp[2]; if (!mid_reader_read(r, tmp, 2)) return false;
    *out = mid_get_u16_be(tmp); return true;
}

static bool mid_reader_read_u32(MidReader *r, uint32_t *out) {
    uint8_t tmp[4]; if (!mid_reader_read(r, tmp, 4)) return false;
    *out = mid_get_u32_be(tmp); return true;
}

static bool mid_reader_read_u64(MidReader *r, uint64_t *out) {
    uint8_t tmp[8]; if (!mid_reader_read(r, tmp, 8)) return false;
    *out = mid_get_u64_be(tmp); return true;
}

static bool mid_key_is_zero(const uint8_t key[32])
{
    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) {
            return false;
        }
    }
    return true;
}

static void mid_xor_fingerprint(uint8_t fp[MID_IDENTITY_KEY_SIZE], const uint8_t key[MID_IDENTITY_KEY_SIZE])
{
    for (size_t i = 0; i < MID_IDENTITY_KEY_SIZE; i++) {
        fp[i] ^= key[i];
    }
}

static bool mid_same_identity(const uint8_t a[MID_IDENTITY_KEY_SIZE],
                              const uint8_t b[MID_IDENTITY_KEY_SIZE])
{
    return memcmp(a, b, MID_IDENTITY_KEY_SIZE) == 0;
}

static bool mid_same_signing_key(const uint8_t a[MID_SIGNING_KEY_SIZE],
                                 const uint8_t b[MID_SIGNING_KEY_SIZE])
{
    return memcmp(a, b, MID_SIGNING_KEY_SIZE) == 0;
}

static bool mid_valid_key_binding(const uint8_t identity_key[MID_IDENTITY_KEY_SIZE],
                                  const uint8_t signing_key[MID_SIGNING_KEY_SIZE])
{
    if (mid_key_is_zero(identity_key) || mid_key_is_zero(signing_key)) {
        printf("[MID] valid_key_binding: rejected, zero key\n"); fflush(stdout);
        return false;
    }

    uint8_t derived_curve_pk[32];
    if (crypto_sign_ed25519_pk_to_curve25519(derived_curve_pk, signing_key) != 0) {
        printf("[MID] valid_key_binding: rejected, curve25519 derivation failed\n"); fflush(stdout);
        return false;
    }

    if (mid_same_identity(identity_key, derived_curve_pk)) {
        printf("[MID] valid_key_binding: accepted, curve25519 derived match\n"); fflush(stdout);
        return true;
    }

    printf("[MID] valid_key_binding: rejected, no match\n"); fflush(stdout);
    return false;
}

static uint32_t mid_chat_id_to_group_number(const Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (tox == NULL || chat_id == NULL) return UINT32_MAX;
    Tox_Err_Group_State_Queries err;
    uint32_t gnum = tox_group_by_chat_id(tox, chat_id, &err);
    if (err != TOX_ERR_GROUP_STATE_QUERIES_OK) {
        return UINT32_MAX;
    }
    return gnum;
}

static MidGroupState *mid_find_group(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (s == NULL || chat_id == NULL) return NULL;
    for (size_t i = 0; i < s->group_count; i++) {
        if (memcmp(s->groups[i].chat_id, chat_id, TOX_GROUP_CHAT_ID_SIZE) == 0) {
            return &s->groups[i];
        }
    }
    return NULL;
}

static const MidGroupState *mid_find_group_const(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    return mid_find_group((MidState *)s, chat_id);
}

static MidGroupState *mid_add_group(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (s == NULL || chat_id == NULL) return NULL;

    if (s->group_count >= s->group_capacity) {
        size_t new_cap = (s->group_capacity == 0) ? 4 : s->group_capacity * 2;
        MidGroupState *tmp = realloc(s->groups, new_cap * sizeof(MidGroupState));
        if (tmp == NULL) return NULL;
        s->groups = tmp;
        s->group_capacity = new_cap;
    }

    MidGroupState *g = &s->groups[s->group_count++];
    memset(g, 0, sizeof(*g));
    memcpy(g->chat_id, chat_id, TOX_GROUP_CHAT_ID_SIZE);
    return g;
}

static bool mid_evict_oldest_offline_active(MidGroupState *g)
{
    if (g == NULL || g->count == 0) {
        return false;
    }

    size_t oldest = g->count;
    uint64_t oldest_seen = 0;

    for (size_t i = 0; i < g->count; i++) {
        const MidPeerRecord *r = &g->records[i];

        /* Only ACTIVE peers that are currently offline are evictable. */
        if (r->status != MID_STATUS_ACTIVE ||
            r->connection_status != TOX_CONNECTION_NONE) {
            continue;
        }

        /* Never evict our own record. */
        if (!mid_key_is_zero(g->self_identity_key) &&
            mid_same_identity(r->identity_key, g->self_identity_key)) {
            continue;
        }

        /* last_seen == 0 is treated as the oldest possible value. */
        if (oldest == g->count || r->last_seen < oldest_seen) {
            oldest = i;
            oldest_seen = r->last_seen;
        }
    }

    if (oldest == g->count) {
        return false;
    }

    printf("[MID] evict_peer: evicting oldest offline ACTIVE peer %zu\n", oldest); fflush(stdout);

    if (g->records[oldest].has_signature) {
        printf("[MID] evict_peer: XOR OUT evicted peer. Old FP[0..3]=%02X%02X%02X%02X\n",
               g->roster_fingerprint[0],
               g->roster_fingerprint[1],
               g->roster_fingerprint[2],
               g->roster_fingerprint[3]);
        fflush(stdout);

        mid_xor_fingerprint(g->roster_fingerprint, g->records[oldest].identity_key);

        printf("[MID] evict_peer: New FP[0..3]=%02X%02X%02X%02X\n",
               g->roster_fingerprint[0],
               g->roster_fingerprint[1],
               g->roster_fingerprint[2],
               g->roster_fingerprint[3]);
        fflush(stdout);
    }

    for (size_t k = oldest; k + 1 < g->count; k++) {
        g->records[k] = g->records[k + 1];
    }

    g->count--;

    return true;
}

static bool mid_ensure_capacity(MidGroupState *g)
{
    if (g == NULL) {
        return false;
    }

    if (g->count >= MID_MAX_PEERS_PER_GROUP) {
        printf("[MID] ensure_capacity: peer limit %d reached, trying eviction\n", MID_MAX_PEERS_PER_GROUP); fflush(stdout);

        if (!mid_evict_oldest_offline_active(g)) {
            printf("[MID] ensure_capacity: rejected, peer limit %d reached and no offline ACTIVE peer to evict\n", MID_MAX_PEERS_PER_GROUP); fflush(stdout);
            return false;
        }

        if (g->count >= MID_MAX_PEERS_PER_GROUP) {
            printf("[MID] ensure_capacity: still at peer limit after eviction\n"); fflush(stdout);
            return false;
        }
    }

    if (g->count < g->capacity) {
        return true;
    }

    size_t new_capacity = (g->capacity == 0) ? 16 : g->capacity * 2;

    if (new_capacity > MID_MAX_PEERS_PER_GROUP) {
        new_capacity = MID_MAX_PEERS_PER_GROUP;
        printf("[MID] ensure_capacity: new_capacity cut to limit\n"); fflush(stdout);
    }

    printf("[MID] ensure_capacity: reallocating from %zu to %zu\n", g->capacity, new_capacity); fflush(stdout);

    MidPeerRecord *tmp = realloc(g->records, new_capacity * sizeof(MidPeerRecord));
    if (tmp == NULL) {
        printf("[MID] ensure_capacity: realloc failed\n"); fflush(stdout);
        return false;
    }

    g->records = tmp;
    g->capacity = new_capacity;
    return true;
}

static int mid_find_identity(const MidGroupState *g,
                             const uint8_t identity_key[MID_IDENTITY_KEY_SIZE])
{
    for (size_t i = 0; i < g->count; i++) {
        if (mid_same_identity(g->records[i].identity_key, identity_key)) {
            return (int)i;
        }
    }
    return -1;
}

/******************************************************************************
Record serialization and crypto
******************************************************************************/

static bool mid_pack_record_body(uint8_t *buf,
                                 size_t cap,
                                 const MidPeerRecord *r,
                                 size_t *out_len)
{
    if (r->nickname_len > MID_MAX_NICK_SIZE) {
        return false;
    }

    if (mid_key_is_zero(r->identity_key)) {
        return false;
    }

    size_t need = MID_RECORD_FIXED_BODY_SIZE + r->nickname_len + MID_IDENTITY_KEY_SIZE + MID_SIGNING_KEY_SIZE;

    if (need > cap) {
        printf("[MID] pack_record_body: rejected, need %zu > cap %zu\n", need, cap); fflush(stdout);
        return false;
    }

    printf("[MID] pack_record_body: packing %zu bytes\n", need); fflush(stdout);

    size_t o = 0;
    buf[o++] = r->status;
    mid_put_u64_be(buf + o, r->timestamp);
    o += 8;
    mid_put_u16_be(buf + o, r->nickname_len);
    o += 2;
    if (r->nickname_len > 0) {
        memcpy(buf + o, r->nickname, r->nickname_len);
        o += r->nickname_len;
    }
    memcpy(buf + o, r->identity_key, MID_IDENTITY_KEY_SIZE);
    o += MID_IDENTITY_KEY_SIZE;
    memcpy(buf + o, r->signing_key, MID_SIGNING_KEY_SIZE);
    o += MID_SIGNING_KEY_SIZE;

    if (out_len != NULL) {
        *out_len = o;
    }
    return true;
}

static bool mid_unpack_record_body(MidPeerRecord *r,
                                   const uint8_t *buf,
                                   size_t len)
{
    memset(r, 0, sizeof(*r));

    size_t o = 0;
    size_t min_len = MID_RECORD_FIXED_BODY_SIZE + MID_IDENTITY_KEY_SIZE + MID_SIGNING_KEY_SIZE;

    if (len < min_len) {
        printf("[MID] unpack_record_body: rejected, len %zu < min_len %zu\n", len, min_len); fflush(stdout);
        return false;
    }

    printf("[MID] unpack_record_body: unpacking %zu bytes\n", len); fflush(stdout);

    r->status = buf[o++];
    if (r->status != MID_STATUS_ACTIVE && r->status != MID_STATUS_LEFT) {
        return false;
    }

    r->timestamp = mid_get_u64_be(buf + o);
    o += 8;

    r->nickname_len = mid_get_u16_be(buf + o);
    o += 2;

    if (r->nickname_len > MID_MAX_NICK_SIZE) {
        return false;
    }

    if (o + r->nickname_len + MID_IDENTITY_KEY_SIZE + MID_SIGNING_KEY_SIZE > len) {
        return false;
    }

    if (r->nickname_len > 0) {
        memcpy(r->nickname, buf + o, r->nickname_len);
        o += r->nickname_len;
    }

    memcpy(r->identity_key, buf + o, MID_IDENTITY_KEY_SIZE);
    o += MID_IDENTITY_KEY_SIZE;

    memcpy(r->signing_key, buf + o, MID_SIGNING_KEY_SIZE);
    o += MID_SIGNING_KEY_SIZE;

    if (mid_key_is_zero(r->identity_key)) {
        return false;
    }

    r->has_signature = false;
    r->connection_status = TOX_CONNECTION_NONE;
    r->role = TOX_GROUP_ROLE_USER;
    r->last_seen = 0;
    return true;
}

static bool mid_verify_record(const MidPeerRecord *r)
{
    if (!r->has_signature) {
        printf("[MID] verify_record: rejected, no signature\n"); fflush(stdout);
        return false;
    }

    if (!mid_valid_key_binding(r->identity_key, r->signing_key)) {
        printf("[MID] verify_record: rejected, invalid key binding\n"); fflush(stdout);
        return false;
    }

    uint8_t body[MID_MAX_BODY_SIZE];
    size_t body_len = 0;
    if (!mid_pack_record_body(body, sizeof(body), r, &body_len)) {
        return false;
    }

    int res = crypto_sign_verify_detached(r->signature,
                                          body,
                                          body_len,
                                          r->signing_key);

    if (res == 0) {
        printf("[MID] verify_record: signature VALID\n"); fflush(stdout);
        return true;
    } else {
        printf("[MID] verify_record: signature INVALID\n"); fflush(stdout);
        return false;
    }
}

static bool mid_sign_record(MidGroupState *g, MidPeerRecord *r)
{
    if (!g->have_keys) {
        printf("[MID] sign_record: failed, no keys\n"); fflush(stdout);
        return false;
    }

    uint8_t body[MID_MAX_BODY_SIZE];
    size_t body_len = 0;
    if (!mid_pack_record_body(body, sizeof(body), r, &body_len)) {
        return false;
    }

    unsigned long long sig_len = 0;
    if (crypto_sign_detached(r->signature,
                             &sig_len,
                             body,
                             body_len,
                             g->self_secret_signing_key) != 0) {
        printf("[MID] sign_record: crypto_sign_detached failed\n"); fflush(stdout);
        return false;
    }

    if (sig_len != MID_SIG_SIZE) {
        printf("[MID] sign_record: unexpected sig_len %llu\n", sig_len); fflush(stdout);
        return false;
    }

    printf("[MID] sign_record: successfully signed %zu bytes\n", body_len); fflush(stdout);
    r->has_signature = true;
    return true;
}

/******************************************************************************
Roster record upsert
Returns true if the roster was actually modified (inserted or updated).

This version maintains g->roster_fingerprint by XORing the 32-byte 
identity_key of every peer that has a valid signature. 

This means the fingerprint tracks the SET of signed peers. It changes when:
  - a signed peer is added
  - a signed peer is removed
  - an unsigned peer becomes signed
  
It does NOT change when a signed peer updates their nickname or timestamp.
******************************************************************************/

static bool mid_upsert_record(MidGroupState *g, const MidPeerRecord *in)
{
    printf("[MID] upsert_record: processing record\n");
    fflush(stdout);

    if (g == NULL || in == NULL) {
        return false;
    }

    MidPeerRecord tmp = *in;

    if (mid_key_is_zero(tmp.identity_key)) {
        printf("[MID] upsert_record: rejected, zero identity key\n");
        fflush(stdout);
        return false;
    }

    if (tmp.status != MID_STATUS_ACTIVE && tmp.status != MID_STATUS_LEFT) {
        printf("[MID] upsert_record: rejected, invalid status\n");
        fflush(stdout);
        return false;
    }

    if (tmp.nickname_len > MID_MAX_NICK_SIZE) {
        printf("[MID] upsert_record: rejected, nickname too long\n");
        fflush(stdout);
        return false;
    }

    /*
     * Unsigned records do not carry a valid persistent signature.
     * Zero the signature field so it cannot accidentally influence later logic.
     */
    if (!tmp.has_signature) {
        memset(tmp.signature, 0, sizeof(tmp.signature));
    }

    /*
     * Verify signed records before touching any state.
     */
    if (tmp.has_signature && !mid_verify_record(&tmp)) {
        printf("[MID] upsert_record: rejected, signature verification failed\n");
        fflush(stdout);
        return false;
    }

    /**************************************************************************
     * SIGNED LEFT TOMBSTONE PATH
     *
     * Signed LEFT tombstones are authoritative departure records.
     * They must be stored and forwarded so offline peers can later learn
     * cryptographically that this identity left.
     *************************************************************************/
    if (tmp.has_signature && tmp.status == MID_STATUS_LEFT) {
        int idx = mid_find_identity(g, tmp.identity_key);

        if (idx >= 0) {
            MidPeerRecord *ex = &g->records[idx];

            /*
             * If we already have a signed record for this identity, the
             * signing key may not change.
             */
            if (ex->has_signature &&
                tmp.has_signature &&
                !mid_same_signing_key(ex->signing_key, tmp.signing_key)) {
                printf("[MID] upsert_record: rejected LEFT tombstone, signing key changed\n");
                fflush(stdout);
                return false;
            }

            /*
             * For tombstones we use >= so that a LEFT tombstone with the
             * same timestamp as the last ACTIVE record still wins.
             */
            if (tmp.timestamp >= ex->timestamp) {
                bool same_signature =
                    ex->has_signature &&
                    tmp.has_signature &&
                    memcmp(ex->signature, tmp.signature, MID_SIG_SIZE) == 0;

                /*
                 * If we already have this exact tombstone and it is already
                 * marked offline, there is nothing to do.
                 */
                if (same_signature && ex->connection_status == TOX_CONNECTION_NONE) {
                    printf("[MID] upsert_record: identical LEFT tombstone already stored\n");
                    fflush(stdout);
                    return false;
                }

                /* 
                 * Track if this peer is transitioning from unsigned to signed.
                 * If they were already signed, they are already in the fingerprint set.
                 */
                bool gained_signature = !ex->has_signature && tmp.has_signature;

                *ex = tmp;
                ex->connection_status = TOX_CONNECTION_NONE;

                if (gained_signature) {
                    printf("[MID] upsert_record: XOR IN tombstone transition. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
                    mid_xor_fingerprint(g->roster_fingerprint, ex->identity_key);
                    printf("[MID] upsert_record: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
                }

                printf("[MID] upsert_record: stored signed LEFT tombstone\n");
                fflush(stdout);

                return true; /* CHANGED */
            }

            /*
             * Older tombstone than our current record. Ignore.
             */
            return false;
        }

        /* Insert new tombstone for unknown peer */
        if (!mid_ensure_capacity(g)) {
            printf("[MID] upsert_record: failed to ensure capacity for tombstone\n");
            fflush(stdout);
            return false;
        }

        g->records[g->count] = tmp;
        g->records[g->count].connection_status = TOX_CONNECTION_NONE;

        if (g->records[g->count].has_signature) {
            printf("[MID] upsert_record: XOR IN new tombstone. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
            mid_xor_fingerprint(g->roster_fingerprint, g->records[g->count].identity_key);
            printf("[MID] upsert_record: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
        }

        g->count++;

        printf("[MID] upsert_record: inserted new signed LEFT tombstone\n");
        fflush(stdout);

        return true; /* CHANGED */
    }

    /**************************************************************************
     * NORMAL ACTIVE / UNSIGNED / SIGNED PATH
     *************************************************************************/

    int idx = mid_find_identity(g, tmp.identity_key);

    if (idx < 0) {
        if (!mid_ensure_capacity(g)) {
            printf("[MID] upsert_record: failed to ensure capacity\n");
            fflush(stdout);
            return false;
        }

        g->records[g->count] = tmp;

        if (g->records[g->count].status == MID_STATUS_LEFT) {
            g->records[g->count].connection_status = TOX_CONNECTION_NONE;
        }

        if (g->records[g->count].has_signature) {
            printf("[MID] upsert_record: XOR IN new peer. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
            mid_xor_fingerprint(g->roster_fingerprint, g->records[g->count].identity_key);
            printf("[MID] upsert_record: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
        }

        g->count++;

        printf("[MID] upsert_record: inserted new peer (count=%zu)\n", g->count);
        fflush(stdout);

        return true; /* CHANGED */
    }

    MidPeerRecord *existing = &g->records[idx];

    /*
     * If we already have a signed record for this identity, the signing key
     * is immutable. Reject records signed by a different key.
     */
    if (existing->has_signature &&
        tmp.has_signature &&
        !mid_same_signing_key(existing->signing_key, tmp.signing_key)) {
        printf("[MID] upsert_record: rejected, signing key changed for same identity\n");
        fflush(stdout);
        return false;
    }

    bool replace = false;

    if (tmp.has_signature) {
        /*
         * Signed records always replace unsigned records.
         * Signed records replace older signed records only if the timestamp
         * is strictly newer.
         */
        if (!existing->has_signature || tmp.timestamp > existing->timestamp) {
            replace = true;
        }
    } else {
        /*
         * Unsigned records only replace other unsigned records.
         * They never replace signed records.
         */
        if (!existing->has_signature && tmp.timestamp >= existing->timestamp) {
            replace = true;
        }
    }

    bool changed = false;

    if (replace) {
        printf("[MID] upsert_record: replacing existing record\n");
        fflush(stdout);

        bool old_had_signature = existing->has_signature;
        bool new_has_signature = tmp.has_signature;

        Tox_Connection old_conn = existing->connection_status;
        Tox_Group_Role old_role = existing->role;
        uint64_t old_last_seen = existing->last_seen;

        /*
         * If the incoming record does not know the signing key but we already
         * have it, preserve it. This only matters for unsigned records.
         */
        if (mid_key_is_zero(tmp.signing_key) &&
            !mid_key_is_zero(existing->signing_key)) {
            memcpy(tmp.signing_key, existing->signing_key, MID_SIGNING_KEY_SIZE);
        }

        /*
         * If the incoming signed ACTIVE record has no local connection state,
         * preserve our previous observation.
         */
        if (tmp.status == MID_STATUS_ACTIVE &&
            tmp.connection_status == TOX_CONNECTION_NONE &&
            old_conn != TOX_CONNECTION_NONE) {
            tmp.connection_status = old_conn;
        }

        /*
         * LEFT records are never online.
         */
        if (tmp.status == MID_STATUS_LEFT) {
            tmp.connection_status = TOX_CONNECTION_NONE;
        }

        /*
         * Role is a local observation. Preserve it unless the caller updates
         * it through moderation / sync logic.
         */
        tmp.role = old_role;

        /*
         * Preserve the newest last_seen value.
         */
        if (old_last_seen > tmp.last_seen) {
            tmp.last_seen = old_last_seen;
        }

        *existing = tmp;

        /*
         * Update fingerprint ONLY if the peer's signature status changed.
         * If they were signed before, and are signed now, they are already 
         * in the XOR set, so we do nothing.
         */
        if (!old_had_signature && new_has_signature) {
            printf("[MID] upsert_record: XOR IN peer transition to signed. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
            mid_xor_fingerprint(g->roster_fingerprint, existing->identity_key);
            printf("[MID] upsert_record: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
        } else if (old_had_signature && !new_has_signature) {
            printf("[MID] upsert_record: XOR OUT peer transition to unsigned. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
            mid_xor_fingerprint(g->roster_fingerprint, existing->identity_key);
            printf("[MID] upsert_record: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
        }

        changed = true;
    } else {
        printf("[MID] upsert_record: keeping existing record\n");
        fflush(stdout);
    }

    /**************************************************************************
     * LOCAL ONLINE STATE UPDATE
     *
     * This updates connection_status / last_seen only.
     * It never modifies signed fields.
     *
     * Important:
     * Do not allow connection updates to resurrect a LEFT tombstone.
     *************************************************************************/
    if (in->connection_status != TOX_CONNECTION_NONE &&
        in->status == MID_STATUS_ACTIVE &&
        existing->status == MID_STATUS_ACTIVE) {

        if (existing->connection_status != in->connection_status) {
            existing->connection_status = in->connection_status;
            changed = true;
        }

        if (in->last_seen >= existing->last_seen) {
            if (in->last_seen > existing->last_seen) {
                existing->last_seen = in->last_seen;

                /*
                 * If you do not want last_seen-only changes to trigger the
                 * peer-list-changed callback, remove the next line.
                 */
                changed = true;
            }
        }
    }

    return changed;
}

/******************************************************************************
Packet sending
******************************************************************************/

static bool mid_send_custom(MidState *s,
                            const Tox *tox,
                            const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                            bool lossless,
                            const uint8_t *data,
                            size_t length)
{
    printf("[MID] send_custom: sending %zu bytes (lossless=%d)\n", length, lossless); fflush(stdout);

    if (tox == NULL || data == NULL || length == 0) {
        return false;
    }

    if (length > MID_MAX_PACKET_SIZE) {
        return false;
    }

    uint32_t group_number = mid_chat_id_to_group_number(tox, chat_id);
    if (group_number == UINT32_MAX) {
        printf("[MID] send_custom: failed to resolve chat_id to group_number\n"); fflush(stdout);
        return false;
    }

    Tox_Err_Group_Send_Custom_Packet err;
    bool ok = tox_group_send_custom_packet(tox,
                                           group_number,
                                           lossless,
                                           data,
                                           length,
                                           &err);

    printf("[MID] send_custom:err=%d ok=%d group_number=%d\n", (int)err, (int)ok, (int)group_number); fflush(stdout);

    if (!ok) {
        printf("[MID] send_custom: tox_group_send_custom_packet failed (err=%d)\n", err); fflush(stdout);
    } else {
        if (s != NULL) {
            s->sent_bytes += length;
        }
    }
    return ok;
}

static bool mid_send_presence_record(MidState *s,
                                     MidGroupState *g,
                                     const Tox *tox,
                                     const MidPeerRecord *r)
{
    if (g == NULL || tox == NULL || r == NULL) {
        return false;
    }

    if (!r->has_signature) {
        return false;
    }

    uint8_t body[MID_MAX_BODY_SIZE];
    size_t body_len = 0;
    if (!mid_pack_record_body(body, sizeof(body), r, &body_len)) {
        return false;
    }

    size_t packet_len = MID_HEADER_SIZE + MID_SIG_SIZE + body_len;
    if (packet_len > MID_MAX_PACKET_SIZE) {
        return false;
    }

    uint8_t packet[MID_MAX_PACKET_SIZE];
    memcpy(packet, MID_MAGIC, MID_MAGIC_BYTES_TOTAL);
    packet[MID_MAGIC_BYTES_TOTAL] = MID_PROTOCOL_VERSION;
    packet[MID_MAGIC_BYTES_TOTAL + 1] = MID_MSG_PRESENCE;

    size_t o = MID_HEADER_SIZE;
    memcpy(packet + o, r->signature, MID_SIG_SIZE);
    o += MID_SIG_SIZE;
    memcpy(packet + o, body, body_len);
    o += body_len;

    return mid_send_custom(s, tox, g->chat_id, true, packet, o);
}

/* FIX 3: Lightweight Heartbeat Sending */
static bool mid_send_heartbeat_record(MidState *s, MidGroupState *g, const Tox *tox)
{
    if (!g || !tox || !g->have_keys) return false;

    uint8_t body[MID_HEARTBEAT_BODY_SIZE];
    size_t o = 0;
    body[o++] = MID_STATUS_ACTIVE;
    mid_put_u64_be(body + o, mid_now_or_time(0));
    o += 8;
    memcpy(body + o, g->self_identity_key, MID_IDENTITY_KEY_SIZE);
    o += MID_IDENTITY_KEY_SIZE;

    uint8_t sig[MID_SIG_SIZE];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, body, o, g->self_secret_signing_key) != 0) return false;

    uint8_t packet[128];
    memcpy(packet, MID_MAGIC, MID_MAGIC_BYTES_TOTAL);
    packet[MID_MAGIC_BYTES_TOTAL] = MID_PROTOCOL_VERSION;
    packet[MID_MAGIC_BYTES_TOTAL + 1] = MID_MSG_HEARTBEAT;

    memcpy(packet + MID_HEADER_SIZE, sig, MID_SIG_SIZE);
    memcpy(packet + MID_HEADER_SIZE + MID_SIG_SIZE, body, o);

    return mid_send_custom(s, tox, g->chat_id, true, packet, MID_HEADER_SIZE + MID_SIG_SIZE + o);
}

static bool mid_send_roster_request_group(MidState *s, MidGroupState *g, const Tox *tox)
{
    printf("[MID] send_roster_request: sending request\n"); fflush(stdout);

    if (g == NULL || tox == NULL) {
        return false;
    }

    uint8_t packet[MID_HEADER_SIZE];
    memcpy(packet, MID_MAGIC, MID_MAGIC_BYTES_TOTAL);
    packet[MID_MAGIC_BYTES_TOTAL] = MID_PROTOCOL_VERSION;
    packet[MID_MAGIC_BYTES_TOTAL + 1] = MID_MSG_ROSTER_REQUEST;

    return mid_send_custom(s, tox, g->chat_id, true, packet, sizeof(packet));
}

static bool mid_send_roster_group(MidState *s, MidGroupState *g, const Tox *tox)
{
    printf("[MID] send_roster: syncing %zu records\n", g->count); fflush(stdout);
    if (g == NULL || tox == NULL) return false;

    printf("[MID] send_roster: Injecting FP[0..3]=%02X%02X%02X%02X into batch header\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);

    uint8_t packet[MID_MAX_PACKET_SIZE];
    size_t p_idx = 0;
    
    memcpy(packet, MID_MAGIC, MID_MAGIC_BYTES_TOTAL);
    packet[MID_MAGIC_BYTES_TOTAL] = MID_PROTOCOL_VERSION;
    packet[MID_MAGIC_BYTES_TOTAL + 1] = MID_MSG_ROSTER_BATCH;
    p_idx = MID_HEADER_SIZE;
    
    // Define sizes dynamically based on actual types/arrays
    const size_t fp_size = sizeof(g->roster_fingerprint);
    const size_t count_size = sizeof(uint16_t);
    
    // Inject Set Fingerprint
    memcpy(packet + p_idx, g->roster_fingerprint, fp_size);
    p_idx += fp_size;
    
    size_t count_idx = p_idx;
    p_idx += count_size;
    uint16_t count = 0;
    bool ok = true;
    
    // Header + Fingerprint + Count
    size_t batch_header_size = MID_HEADER_SIZE + fp_size + count_size;

    for (size_t i = 0; i < g->count; i++) {
        if (!g->records[i].has_signature) continue;
        
        uint8_t body[MID_MAX_BODY_SIZE];
        size_t body_len = 0;
        if (!mid_pack_record_body(body, sizeof(body), &g->records[i], &body_len)) continue;
        
        // Signature + body_len prefix + body
        size_t needed = MID_SIG_SIZE + count_size + body_len;

        if (p_idx + needed > MID_MAX_PACKET_SIZE) {
            mid_put_u16_be(packet + count_idx, count);
            if (!mid_send_custom(s, tox, g->chat_id, true, packet, p_idx)) ok = false;
            
            p_idx = batch_header_size; 
            printf("[MID] send_roster: Re-injecting FP[0..3]=%02X%02X%02X%02X into next batch packet\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
            
            // Re-inject full header for next packet
            memcpy(packet, MID_MAGIC, MID_MAGIC_BYTES_TOTAL);
            packet[MID_MAGIC_BYTES_TOTAL] = MID_PROTOCOL_VERSION;
            packet[MID_MAGIC_BYTES_TOTAL + 1] = MID_MSG_ROSTER_BATCH;
            memcpy(packet + MID_HEADER_SIZE, g->roster_fingerprint, fp_size);
            
            count_idx = MID_HEADER_SIZE + fp_size;
            count = 0;
        }
        
        memcpy(packet + p_idx, g->records[i].signature, MID_SIG_SIZE);
        p_idx += MID_SIG_SIZE;
        mid_put_u16_be(packet + p_idx, (uint16_t)body_len);
        p_idx += count_size;
        memcpy(packet + p_idx, body, body_len);
        p_idx += body_len;
        count++;
    }
    
    if (count > 0) {
        mid_put_u16_be(packet + count_idx, count);
        if (!mid_send_custom(s, tox, g->chat_id, true, packet, p_idx)) ok = false;
    }
    
    return ok;
}

/******************************************************************************
Online state sync
Returns true if any peer's online status changed.
******************************************************************************/

/*
Stamps the FOUNDER role on the roster entry whose identity_key matches
the group founder's public key obtained from Toxcore's shared state.

This works regardless of whether the founder is currently online, because
the founder key is part of the group's cryptographic shared state, not
the live peer list.
*/
static void mid_apply_founder_role(MidGroupState *g, const Tox *tox)
{
    if (g == NULL || tox == NULL) {
        return;
    }

    uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
    if (group_number == UINT32_MAX) {
        return;
    }

    uint8_t founder_identity[MID_IDENTITY_KEY_SIZE];
    Tox_Err_Group_State_Queries err;

    if (!tox_group_get_founder_public_key(tox, group_number, founder_identity, &err)) {
        return;
    }

    if (mid_key_is_zero(founder_identity)) {
        return;
    }

    int idx = mid_find_identity(g, founder_identity);
    if (idx < 0) {
        return;
    }

    if (g->records[idx].role != TOX_GROUP_ROLE_FOUNDER) {
        printf("[MID] apply_founder_role: stamping FOUNDER role on peer %zu\n", (size_t)idx);
        fflush(stdout);
        g->records[idx].role = TOX_GROUP_ROLE_FOUNDER;
    }
}

static bool mid_sync_online_state_group(MidGroupState *g, const Tox *tox, uint64_t now)
{
    if (g == NULL || tox == NULL) {
        return false;
    }

    now = mid_now_or_time(now);
    bool changed = false;

    uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
    if (group_number == UINT32_MAX) return false;

    /* Ensure the founder role is always correctly stamped */
    Tox_Group_Role old_founder_role = TOX_GROUP_ROLE_USER;
    int founder_idx = -1;
    uint8_t founder_identity[MID_IDENTITY_KEY_SIZE];
    Tox_Err_Group_State_Queries founder_err;

    if (tox_group_get_founder_public_key(tox, group_number, founder_identity, &founder_err)) {
        founder_idx = mid_find_identity(g, founder_identity);
        if (founder_idx >= 0) {
            old_founder_role = g->records[founder_idx].role;
        }
    }

    mid_apply_founder_role(g, tox);

    if (founder_idx >= 0 && g->records[founder_idx].role != old_founder_role) {
        changed = true;
    }

    for (size_t i = 0; i < g->count; i++) {
        MidPeerRecord *e = &g->records[i];
        if (e->connection_status == TOX_CONNECTION_NONE) {
            continue;
        }

        Tox_Err_Group_Peer_Query err;
        uint32_t peer_id = tox_group_peer_by_public_key(tox, group_number, e->identity_key, &err);

        if (err != TOX_ERR_GROUP_PEER_QUERY_OK) {
            printf("[MID] sync_online_state: peer %zu is no longer in Toxcore, marking offline\n", i); fflush(stdout);
            if (e->connection_status != TOX_CONNECTION_NONE) {
                e->connection_status = TOX_CONNECTION_NONE;
                changed = true;
            }
            if (now >= e->last_seen) {
                e->last_seen = now;
            }
        } else {
            Tox_Err_Group_Peer_Query conn_err;
            Tox_Connection conn = tox_group_peer_get_connection_status(tox, group_number, peer_id, &conn_err);
            if (conn_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
                if (e->connection_status != conn) {
                    e->connection_status = conn;
                    changed = true;
                }
                if (conn != TOX_CONNECTION_NONE && now >= e->last_seen) {
                    e->last_seen = now;
                }
            }

            Tox_Err_Group_Peer_Query role_err;
            Tox_Group_Role role = tox_group_peer_get_role(tox, group_number, peer_id, &role_err);
            if (role_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
                if (e->role != role) {
                    e->role = role;
                    changed = true;
                }
            }
        }
    }

    return changed;
}

/******************************************************************************
Key setup
******************************************************************************/

static bool mid_set_self_keys(MidGroupState *g,
                              const uint8_t identity_key[MID_IDENTITY_KEY_SIZE],
                              const uint8_t signing_key[MID_SIGNING_KEY_SIZE],
                              const uint8_t secret_key[MID_SIGNING_SECRET_KEY_SIZE])
{
    printf("[MID] set_self_keys: setting keys\n"); fflush(stdout);

    if (g == NULL ||
        identity_key == NULL ||
        signing_key == NULL ||
        secret_key == NULL) {
        return false;
    }

    if (mid_key_is_zero(identity_key) || mid_key_is_zero(signing_key)) {
        return false;
    }

    uint8_t derived_signing_key[MID_SIGNING_KEY_SIZE];
    if (crypto_sign_ed25519_sk_to_pk(derived_signing_key, secret_key) != 0) {
        printf("[MID] set_self_keys: sk_to_pk failed\n"); fflush(stdout);
        return false;
    }

    if (!mid_same_signing_key(derived_signing_key, signing_key)) {
        printf("[MID] set_self_keys: derived signing key mismatch\n"); fflush(stdout);
        return false;
    }

    if (!mid_valid_key_binding(identity_key, signing_key)) {
        printf("[MID] set_self_keys: invalid key binding\n"); fflush(stdout);
        return false;
    }

    memcpy(g->self_identity_key, identity_key, MID_IDENTITY_KEY_SIZE);
    memcpy(g->self_signing_key, signing_key, MID_SIGNING_KEY_SIZE);
    memcpy(g->self_secret_signing_key, secret_key, MID_SIGNING_SECRET_KEY_SIZE);
    g->have_keys = true;

    printf("[MID] set_self_keys: keys set successfully\n"); fflush(stdout);
    return true;
}

static bool mid_init_self_from_tox(MidGroupState *g, const Tox *tox)
{
    if (g == NULL || tox == NULL) {
        return false;
    }

    if (g->have_keys) {
        return true;
    }

    printf("[MID] init_self_from_tox: fetching keys from Toxcore\n"); fflush(stdout);

    uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
    if (group_number == UINT32_MAX) {
        return false;
    }

    uint8_t identity[MID_IDENTITY_KEY_SIZE];
    uint8_t signing[MID_SIGNING_KEY_SIZE];
    uint8_t sk[MID_SIGNING_SECRET_KEY_SIZE];

    Tox_Err_Group_Self_Query err;

    if (!tox_group_self_get_public_key(tox, group_number, identity, &err)) {
        printf("[MID] init_self_from_tox: get_public_key failed\n"); fflush(stdout);
        return false;
    }

    printf("[MID] init_self_from_tox: got identity key\n");
    fflush(stdout);

    /*
     * We now have the current Toxcore identity in the local variable
     * "identity".
     *
     * g->self_identity_key may contain the public identity that was loaded
     * from the middleware save file.
     *
     * This comparison must happen BEFORE mid_set_self_keys(), because
     * mid_set_self_keys() will overwrite g->self_identity_key.
     */
    if (!mid_key_is_zero(g->self_identity_key) &&
        !mid_same_identity(identity, g->self_identity_key)) {

        printf("[MID] init_self_from_tox: Toxcore identity differs from saved middleware identity\n"); fflush(stdout);

        /*
         * Choose your policy here.
         *
         * Option 1: Strict mode.
         *
         * If the saved middleware state must belong to exactly the same
         * identity that Toxcore currently has, fail here.
         *
         * return false;
         *
         *
         * Option 2: Adopt the current Toxcore identity.
         *
         * Toxcore is the authority for the current group identity.
         * Continue and let mid_set_self_keys() install the keys reported
         * by Toxcore.
         *
         * This is usually the more robust choice.
         */
    }

    if (!tox_group_self_get_signing_public_key(tox, group_number, signing, &err)) {
        printf("[MID] init_self_from_tox: get_signing_public_key failed\n"); fflush(stdout);
        return false;
    }
    printf("[MID] init_self_from_tox: got signing public key\n"); fflush(stdout);

    if (!tox_group_self_get_signing_secret_key(tox, group_number, sk, &err)) {
        printf("[MID] init_self_from_tox: get_signing_secret_key failed\n"); fflush(stdout);
        return false;
    }
    printf("[MID] init_self_from_tox: got signing secret key\n"); fflush(stdout);

    bool ok = mid_set_self_keys(g, identity, signing, sk);
    sodium_memzero(sk, sizeof(sk));
    return ok;
}

/******************************************************************************
Tox peer key helpers
******************************************************************************/

static bool mid_get_peer_keys(const Tox *tox,
                              const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                              uint32_t peer_id,
                              uint8_t identity_key[MID_IDENTITY_KEY_SIZE],
                              uint8_t signing_key[MID_SIGNING_KEY_SIZE])
{
    printf("[MID] get_peer_keys: fetching keys for peer %u\n", peer_id); fflush(stdout);

    if (tox == NULL || identity_key == NULL || signing_key == NULL) {
        return false;
    }

    uint32_t group_number = mid_chat_id_to_group_number(tox, chat_id);
    if (group_number == UINT32_MAX) return false;

    Tox_Err_Group_Peer_Query err;

    if (!tox_group_peer_get_public_key(tox, group_number, peer_id,
                                       identity_key, &err)) {
        printf("[MID] get_peer_keys: get_public_key failed for peer %u\n", peer_id); fflush(stdout);
        return false;
    }

    if (!tox_group_peer_get_signing_public_key(tox, group_number, peer_id,
                                               signing_key, &err)) {
        printf("[MID] get_peer_keys: get_signing_public_key failed for peer %u\n", peer_id); fflush(stdout);
        return false;
    }

    printf("[MID] get_peer_keys: got both keys for peer %u\n", peer_id); fflush(stdout);
    return mid_valid_key_binding(identity_key, signing_key);
}

/******************************************************************************
Internal group operations
******************************************************************************/

static bool mid_announce_self_group(MidState *s, MidGroupState *g, const Tox *tox)
{
    printf("[MID] announce_self: announcing ACTIVE\n"); fflush(stdout);

    if (g == NULL) {
        return false;
    }

    if (!g->have_keys) {
        return false;
    }

    uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
    if (group_number == UINT32_MAX) return false;

    MidPeerRecord r;
    memset(&r, 0, sizeof(r));
    memcpy(r.identity_key, g->self_identity_key, MID_IDENTITY_KEY_SIZE);
    memcpy(r.signing_key, g->self_signing_key, MID_SIGNING_KEY_SIZE);
    r.status = MID_STATUS_ACTIVE;
    r.timestamp = mid_now_or_time(0);

    Tox_Err_Group_Peer_Query conn_err;
    Tox_Connection conn = tox_group_peer_get_connection_status(tox, group_number, 0, &conn_err);
    if (conn_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
        r.connection_status = conn;
    } else {
        r.connection_status = TOX_CONNECTION_NONE;
    }

    Tox_Err_Group_Self_Query self_role_err;
    r.role = tox_group_self_get_role(tox, group_number, &self_role_err);
    if (self_role_err != TOX_ERR_GROUP_SELF_QUERY_OK) {
        r.role = TOX_GROUP_ROLE_USER;
    }

    /*
     * Copy self nickname into the record.
     *
     * self_nickname_len was already bounded to MID_MAX_NICK_SIZE when it was
     * stored in mid_on_group_self_join, but we add a defensive bounds check
     * here as well to guard against any corruption.
     */
    if (g->self_nickname_len > 0) {
        uint16_t nick_copy = g->self_nickname_len;
        if (nick_copy > MID_MAX_NICK_SIZE) {
            nick_copy = MID_MAX_NICK_SIZE;
        }
        memcpy(r.nickname, g->self_nickname, nick_copy);
        r.nickname_len = nick_copy;
    }

    if (!mid_sign_record(g, &r)) {
        return false;
    }

    r.last_seen = r.timestamp;

    bool changed = mid_upsert_record(g, &r);
    g->self_active = true;
    g->last_announce = r.timestamp;

    mid_send_presence_record(s, g, tox, &r);
    return changed;
}

static bool mid_on_peer_online_keys(MidGroupState *g,
                                    const uint8_t identity_key[MID_IDENTITY_KEY_SIZE],
                                    const uint8_t signing_key[MID_SIGNING_KEY_SIZE],
                                    Tox_Connection conn,
                                    Tox_Group_Role role,
                                    uint64_t now)
{
    printf("[MID] on_peer_online_keys: peer observed online\n"); fflush(stdout);

    if (g == NULL || identity_key == NULL || mid_key_is_zero(identity_key)) {
        return false;
    }

    now = mid_now_or_time(now);

    bool have_signing = (signing_key != NULL &&
                         !mid_key_is_zero(signing_key) &&
                         mid_valid_key_binding(identity_key, signing_key));

    int idx = mid_find_identity(g, identity_key);

    if (idx < 0) {
        if (!mid_ensure_capacity(g)) {
            return false;
        }
        MidPeerRecord r;
        memset(&r, 0, sizeof(r));
        memcpy(r.identity_key, identity_key, MID_IDENTITY_KEY_SIZE);
        if (have_signing) {
            memcpy(r.signing_key, signing_key, MID_SIGNING_KEY_SIZE);
        }
        r.status = MID_STATUS_ACTIVE;
        r.timestamp = now;
        r.connection_status = conn;
        r.role = role;
        r.last_seen = now;
        g->records[g->count] = r;
        g->count++;
        return true; /* CHANGED */
    }

    MidPeerRecord *e = &g->records[idx];
    bool changed = false;

    if (e->connection_status != conn) {
        e->connection_status = conn;
        changed = true;
    }

    if (e->role != role) {
        e->role = role;
        changed = true;
    }

    if (conn != TOX_CONNECTION_NONE && now >= e->last_seen) {
        e->last_seen = now;
        changed = true;
    }

    if (!e->has_signature) {
        e->status = MID_STATUS_ACTIVE;
        if (now >= e->timestamp) {
            e->timestamp = now;
            changed = true;
        }
        if (have_signing) {
            memcpy(e->signing_key, signing_key, MID_SIGNING_KEY_SIZE);
            changed = true;
        }
    }

    return changed;
}

static bool mid_refresh_peer_name_group(MidGroupState *g, const Tox *tox, uint32_t peer_id)
{
    if (g == NULL || tox == NULL) {
        return false;
    }

    uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
    if (group_number == UINT32_MAX) return false;

    uint8_t identity[MID_IDENTITY_KEY_SIZE];
    Tox_Err_Group_Peer_Query err;

    if (!tox_group_peer_get_public_key(tox, group_number, peer_id, identity, &err)) {
        return false;
    }

    int idx = mid_find_identity(g, identity);
    if (idx < 0) {
        return false;
    }

    size_t name_size = tox_group_peer_get_name_size(tox, group_number, peer_id, &err);
    if (err != TOX_ERR_GROUP_PEER_QUERY_OK || name_size == 0) {
        return false;
    }

    /*
     * SAFETY: tox_group_peer_get_name() writes exactly name_size bytes into
     * the output buffer. We must NOT truncate name_size before calling it,
     * because the buffer must be large enough for the full write.
     *
     * We heap-allocate a buffer of exactly name_size bytes to guarantee no
     * overflow even if Toxcore returns a name larger than MID_MAX_NICK_SIZE
     * (e.g. from a patched or broken Toxcore).
     */
    uint8_t *name = (uint8_t *)malloc(name_size);
    if (name == NULL) {
        return false;
    }

    if (!tox_group_peer_get_name(tox, group_number, peer_id, name, &err)) {
        free(name);
        return false;
    }

    MidPeerRecord *e = &g->records[idx];
    bool changed = false;

    if (!e->has_signature) {
        size_t copy_len = name_size;
        if (copy_len > MID_MAX_NICK_SIZE) {
            copy_len = MID_MAX_NICK_SIZE;
        }

        /* Only update if the name actually changed */
        if (e->nickname_len != copy_len || memcmp(e->nickname, name, copy_len) != 0) {
            /*
             * Zero-fill the destination first so no stale bytes remain past
             * the end of the (possibly truncated) nickname.
             *
             * The nickname is treated as opaque binary. We do NOT assume
             * valid UTF-8, printable ASCII, or NUL termination.
             */
            memset(e->nickname, 0, MID_MAX_NICK_SIZE);
            if (copy_len > 0) {
                memcpy(e->nickname, name, copy_len);
            }
            e->nickname_len = (uint16_t)copy_len;
            changed = true;
        }
    }

    free(name);
    return changed;
}

static bool mid_on_custom_packet_group(MidState *s,
                                       MidGroupState *g,
                                       const Tox *tox,
                                       uint32_t peer_id,
                                       const uint8_t *data,
                                       size_t length,
                                       uint64_t now)
{
    printf("[MID] on_custom_packet: received %zu bytes from peer %u\n",
           length, peer_id);
    fflush(stdout);

    if (g == NULL || data == NULL || length < MID_HEADER_SIZE) {
        return false;
    }

    if (memcmp(data, MID_MAGIC, MID_MAGIC_BYTES_TOTAL) != 0 ||
        data[MID_MAGIC_BYTES_TOTAL] != MID_PROTOCOL_VERSION) {
        return false;
    }

    uint8_t type = data[MID_MAGIC_BYTES_TOTAL + 1];
    const uint8_t *payload = data + MID_HEADER_SIZE;
    size_t payload_len = length - MID_HEADER_SIZE;

    now = mid_now_or_time(now);

    uint8_t sender_identity[MID_IDENTITY_KEY_SIZE];
    uint8_t sender_signing[MID_SIGNING_KEY_SIZE];
    bool sender_has_keys = false;

    if (tox != NULL && peer_id != UINT32_MAX) {
        sender_has_keys = mid_get_peer_keys(tox,
                                            g->chat_id,
                                            peer_id,
                                            sender_identity,
                                            sender_signing);
    }

    bool changed = false;

    /**************************************************************************
     * FULL SIGNED PRESENCE RECORD
     *
     * This is a full signed record containing status, timestamp, nickname,
     * identity key, and signing key.
     *
     * It does NOT suppress pending roster responses, because one presence
     * record does not prove that the sender has the full roster.
     *************************************************************************/
    if (type == MID_MSG_PRESENCE) {
        printf("[MID] on_custom_packet: processing PRESENCE message\n");
        fflush(stdout);

        if (payload_len < MID_SIG_SIZE) {
            return false;
        }

        size_t body_len = payload_len - MID_SIG_SIZE;

        if (body_len > MID_MAX_BODY_SIZE) {
            return false;
        }

        MidPeerRecord r;
        memset(&r, 0, sizeof(r));

        if (!mid_unpack_record_body(&r, payload + MID_SIG_SIZE, body_len)) {
            return false;
        }

        memcpy(r.signature, payload, MID_SIG_SIZE);
        r.has_signature = true;

        if (!mid_verify_record(&r)) {
            printf("[MID] on_custom_packet: PRESENCE verification failed\n");
            fflush(stdout);
            return false;
        }

        printf("[MID] on_custom_packet: PRESENCE verified, upserting\n");
        fflush(stdout);

        /* Never process our own signed presence echoed back to us. */
        if (g->have_keys && mid_same_identity(r.identity_key, g->self_identity_key)) {
            return false;
        }

        bool sender_is_subject = false;

        if (sender_has_keys && mid_same_identity(sender_identity, r.identity_key)) {
            sender_is_subject = true;

            /*
             * If the packet came directly from the subject, the signing key
             * observed from Toxcore must match the signing key in the record.
             */
            if (!mid_same_signing_key(sender_signing, r.signing_key)) {
                return false;
            }
        }

        if (sender_is_subject && r.status == MID_STATUS_ACTIVE) {
            Tox_Err_Group_Peer_Query conn_err;
            Tox_Connection conn = TOX_CONNECTION_NONE;

            uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
            if (group_number != UINT32_MAX && tox != NULL && peer_id != UINT32_MAX) {
                conn = tox_group_peer_get_connection_status(tox,
                                                            group_number,
                                                            peer_id,
                                                            &conn_err);
                if (conn_err != TOX_ERR_GROUP_PEER_QUERY_OK) {
                    conn = TOX_CONNECTION_NONE;
                }
            }

            /*
             * They just sent a packet, so they are online from our point of
             * view even if Toxcore has not updated the connection state yet.
             */
            if (conn == TOX_CONNECTION_NONE) {
                conn = TOX_CONNECTION_TCP;
            }

            r.connection_status = conn;
            r.last_seen = now;
        } else {
            /*
             * Forwarded / relayed presence, or LEFT tombstone.
             * Do not mark the peer online.
             */
            r.connection_status = TOX_CONNECTION_NONE;
            /* Anchor last_seen so we can purge them if they never come online */
            if (r.last_seen == 0) r.last_seen = now;
        }

        if (mid_upsert_record(g, &r)) {
            changed = true;
        }

        return changed;
    }

    /**************************************************************************
     * LIGHTWEIGHT HEARTBEAT
     *
     * Body:
     *   status       1 byte
     *   timestamp    8 bytes
     *   identity_key 32 bytes
     *
     * Signature:
     *   detached Ed25519 signature over the 41-byte body.
     *
     * Heartbeats are used only for liveness / connection state.
     *
     * IMPORTANT:
     * A heartbeat must NOT modify fields belonging to a stored full signed
     * record, otherwise the stored signature would become invalid.
     * Therefore, if we already have a signed full record, the heartbeat only
     * updates connection_status, role, and last_seen.
     *
     * Heartbeats do NOT suppress roster responses.
     *************************************************************************/
    if (type == MID_MSG_HEARTBEAT) {
        printf("[MID] on_custom_packet: processing HEARTBEAT message\n");
        fflush(stdout);

        const size_t hb_body_len = MID_HEARTBEAT_BODY_SIZE;

        if (payload_len < MID_SIG_SIZE + hb_body_len) {
            return false;
        }

        const uint8_t *sig = payload;
        const uint8_t *body = payload + MID_SIG_SIZE;

        uint8_t status = body[0];

        if (status != MID_STATUS_ACTIVE && status != MID_STATUS_LEFT) {
            return false;
        }

        uint64_t ts = mid_get_u64_be(body + 1);

        uint8_t hb_identity[MID_IDENTITY_KEY_SIZE];
        memcpy(hb_identity, body + 9, MID_IDENTITY_KEY_SIZE);

        /* Ignore our own heartbeat if it is echoed back. */
        if (g->have_keys && mid_same_identity(hb_identity, g->self_identity_key)) {
            return false;
        }

        int idx = mid_find_identity(g, hb_identity);
        if (idx < 0) {
            /*
             * We do not know this peer yet.
             * A heartbeat alone is not enough to create a persistent roster
             * entry; wait for a full signed PRESENCE record.
             */
            return false;
        }

        MidPeerRecord *e = &g->records[idx];

        bool sender_is_subject = false;
        if (sender_has_keys && mid_same_identity(sender_identity, hb_identity)) {
            sender_is_subject = true;
        }

        uint8_t verify_key[MID_SIGNING_KEY_SIZE];
        bool have_verify_key = false;

        /*
         * Prefer the signing key already stored in the roster record.
         */
        if (!mid_key_is_zero(e->signing_key) &&
            mid_valid_key_binding(e->identity_key, e->signing_key)) {
            memcpy(verify_key, e->signing_key, MID_SIGNING_KEY_SIZE);
            have_verify_key = true;
        }
        /*
         * If we do not have a stored signing key yet, but the packet comes
         * directly from the subject, use the signing key observed from
         * Toxcore.
         */
        else if (sender_is_subject &&
                 mid_valid_key_binding(sender_identity, sender_signing)) {
            memcpy(verify_key, sender_signing, MID_SIGNING_KEY_SIZE);
            have_verify_key = true;
        }

        if (!have_verify_key) {
            return false;
        }

        if (crypto_sign_verify_detached(sig,
                                        body,
                                        hb_body_len,
                                        verify_key) != 0) {
            printf("[MID] on_custom_packet: HEARTBEAT signature invalid\n");
            fflush(stdout);
            return false;
        }

        printf("[MID] on_custom_packet: HEARTBEAT signature valid\n");
        fflush(stdout);

        /*
         * Heartbeats are intended as ACTIVE keep-alives.
         * A LEFT state should be propagated by a full signed LEFT tombstone,
         * not by a lightweight heartbeat.
         */
        if (status != MID_STATUS_ACTIVE) {
            return changed;
        }

        /*
         * If we only have an unsigned local observation, it is safe to update
         * the unsigned logical fields.
         */
        if (!e->has_signature) {
            if (mid_key_is_zero(e->signing_key)) {
                memcpy(e->signing_key, verify_key, MID_SIGNING_KEY_SIZE);
                changed = true;
            }

            if (e->status != MID_STATUS_ACTIVE) {
                e->status = MID_STATUS_ACTIVE;
                changed = true;
            }

            if (ts > e->timestamp) {
                e->timestamp = ts;
                changed = true;
            }
        } else {
            /*
             * If we have a signed LEFT tombstone, do not resurrect the peer
             * from a heartbeat alone. Require a full signed ACTIVE presence.
             */
            if (e->status == MID_STATUS_LEFT) {
                return changed;
            }
        }

        /*
         * Only the direct sender can be considered online because of this
         * heartbeat. A relayed heartbeat does not prove current connectivity.
         */
        if (sender_is_subject) {
            uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
            if (group_number == UINT32_MAX) return changed;

            Tox_Err_Group_Peer_Query conn_err;
            Tox_Connection conn = tox_group_peer_get_connection_status(tox,
                                                                       group_number,
                                                                       peer_id,
                                                                       &conn_err);

            if (conn_err != TOX_ERR_GROUP_PEER_QUERY_OK) {
                conn = TOX_CONNECTION_NONE;
            }

            if (conn == TOX_CONNECTION_NONE) {
                conn = TOX_CONNECTION_TCP;
            }

            if (e->connection_status != conn) {
                e->connection_status = conn;
                changed = true;
            }

            Tox_Err_Group_Peer_Query role_err;
            Tox_Group_Role role = tox_group_peer_get_role(tox,
                                                          group_number,
                                                          peer_id,
                                                          &role_err);

            if (role_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
                if (e->role != role) {
                    e->role = role;
                    changed = true;
                }
            }

            if (now >= e->last_seen) {
                e->last_seen = now;
                /*
                 * Intentionally not setting changed=true only for last_seen.
                 * This avoids excessive peer-list-changed callbacks when only
                 * the last-seen timestamp moves forward.
                 */
            }
        }

        return changed;
    }

    /**************************************************************************
     * BATCHED ROSTER RESPONSE
     *
     * Payload layout:
     *   fingerprint  32 bytes
     *   count         2 bytes
     *   records...
     *
     * Each record:
     *   signature    64 bytes
     *   body_len      2 bytes
     *   body          body_len bytes
     *
     * Suppression rule:
     *
     * We cancel our own pending roster response ONLY if the batch is complete
     * and the sender's roster fingerprint exactly matches ours.
     *
     * This prevents:
     *   - a partial peer list from suppressing a fuller list
     *   - a different set of the same size from suppressing another set
     *   - stale state from suppressing newer state if the fingerprint includes
     *     record signatures / versions
     *************************************************************************/
    if (type == MID_MSG_ROSTER_BATCH) {
        printf("[MID] on_custom_packet: processing ROSTER_BATCH message\n");
        fflush(stdout);

        /* Minimum: MID_IDENTITY_KEY_SIZE fingerprint + 2-byte record count */
        if (payload_len < MID_IDENTITY_KEY_SIZE + sizeof(uint16_t)) {
            return false;
        }

        uint8_t sender_fp[MID_IDENTITY_KEY_SIZE];
        memcpy(sender_fp, payload, MID_IDENTITY_KEY_SIZE);

        uint16_t record_count = mid_get_u16_be(payload + MID_IDENTITY_KEY_SIZE);
        size_t p_idx = MID_IDENTITY_KEY_SIZE + sizeof(uint16_t);

        bool batch_complete = true;

        for (uint16_t i = 0; i < record_count; i++) {
            if (p_idx + MID_SIG_SIZE + sizeof(uint16_t) > payload_len) {
                batch_complete = false;
                break;
            }

            const uint8_t *sig = payload + p_idx;
            p_idx += MID_SIG_SIZE;

            uint16_t body_len = mid_get_u16_be(payload + p_idx);
            p_idx += sizeof(uint16_t);

            if (p_idx + body_len > payload_len) {
                batch_complete = false;
                break;
            }

            /*
             * Defensive limit. The current protocol body cannot exceed
             * MID_MAX_BODY_SIZE.
             */
            if (body_len > MID_MAX_BODY_SIZE) {
                p_idx += body_len;
                continue;
            }

            MidPeerRecord r;
            memset(&r, 0, sizeof(r));

            if (mid_unpack_record_body(&r, payload + p_idx, body_len)) {
                memcpy(r.signature, sig, MID_SIG_SIZE);
                r.has_signature = true;

                if (mid_verify_record(&r)) {
                    /* Ignore our own record if it is echoed back. */
                    if (!(g->have_keys &&
                          mid_same_identity(r.identity_key, g->self_identity_key))) {

                        bool sender_is_subject = false;

                        if (sender_has_keys &&
                            mid_same_identity(sender_identity, r.identity_key)) {
                            sender_is_subject = true;
                        }

                        /*
                         * If the packet sender claims to be the subject, their
                         * Toxcore-observed signing key must match the record.
                         */
                        if (sender_is_subject &&
                            !mid_same_signing_key(sender_signing, r.signing_key)) {
                            sender_is_subject = false;
                        }

                        if (sender_is_subject && r.status == MID_STATUS_ACTIVE) {
                            uint32_t group_number = mid_chat_id_to_group_number(tox, g->chat_id);
                            Tox_Connection conn = TOX_CONNECTION_NONE;
                            Tox_Group_Role role = TOX_GROUP_ROLE_USER;

                            if (group_number != UINT32_MAX && tox != NULL && peer_id != UINT32_MAX) {
                                Tox_Err_Group_Peer_Query conn_err;
                                conn = tox_group_peer_get_connection_status(tox,
                                                                            group_number,
                                                                            peer_id,
                                                                            &conn_err);

                                if (conn_err != TOX_ERR_GROUP_PEER_QUERY_OK) {
                                    conn = TOX_CONNECTION_NONE;
                                }

                                if (conn == TOX_CONNECTION_NONE) {
                                    conn = TOX_CONNECTION_TCP;
                                }

                                Tox_Err_Group_Peer_Query role_err;
                                role = tox_group_peer_get_role(tox,
                                                               group_number,
                                                               peer_id,
                                                               &role_err);

                                if (role_err != TOX_ERR_GROUP_PEER_QUERY_OK) {
                                    role = TOX_GROUP_ROLE_USER;
                                }
                            }

                            r.connection_status = conn;
                            r.last_seen = now;
                            r.role = role;
                        } else {
                            r.connection_status = TOX_CONNECTION_NONE;
                            /* Anchor last_seen so we can purge them if they never come online */
                            if (r.last_seen == 0) r.last_seen = now;
                        }

                        if (mid_upsert_record(g, &r)) {
                            changed = true;
                        }
                    }
                }
            }

            p_idx += body_len;
        }

        /*
         * SAFE MULTICAST SUPPRESSION
         *
         * Only suppress if the batch was structurally complete and the
         * fingerprint matches exactly.
         */
        if (batch_complete) {
            printf("[MID] ROSTER_BATCH: Sender FP[0..3]=%02X%02X%02X%02X, Our FP[0..3]=%02X%02X%02X%02X\n",
                   sender_fp[0], sender_fp[1], sender_fp[2], sender_fp[3],
                   g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]);
            fflush(stdout);
            if (memcmp(sender_fp, g->roster_fingerprint, MID_IDENTITY_KEY_SIZE) == 0) {
                if (g->roster_reply_deadline != 0) {
                    printf("[MID] ROSTER_BATCH: MATCH! Suppressing our pending roster reply because another peer already sent the exact same set.\n");
                    fflush(stdout);
                    g->roster_reply_deadline = 0;
                } else {
                    printf("[MID] ROSTER_BATCH: MATCH! But we had no pending reply to suppress.\n");
                    fflush(stdout);
                }
            } else {
                printf("[MID] ROSTER_BATCH: MISMATCH. Sender has a different peer set. Keeping our pending roster reply scheduled.\n");
                fflush(stdout);
            }
        } else {
            printf("[MID] ROSTER_BATCH: batch incomplete/truncated. "
                   "Not suppressing roster reply.\n");
            fflush(stdout);
        }

        return changed;
    }

    /**************************************************************************
     * ROSTER REQUEST
     *
     * We do not answer immediately. We schedule a delayed response.
     *
     * If another peer responds first with a roster whose fingerprint matches
     * ours, our pending response will be cancelled by the ROSTER_BATCH
     * suppression logic above.
     *************************************************************************/
    if (type == MID_MSG_ROSTER_REQUEST) {
        printf("[MID] on_custom_packet: processing ROSTER_REQUEST message\n");
        fflush(stdout);

        if (tox == NULL) {
            return false;
        }

        /*
         * Ignore our own request if it is ever echoed back.
         */
        if (sender_has_keys &&
            g->have_keys &&
            mid_same_identity(sender_identity, g->self_identity_key)) {
            return false;
        }

        /*
         * Only respond if we have joined/announced ourselves and have keys.
         */
        if (g->have_keys && g->announced) {
            if (g->roster_reply_deadline == 0) {
                /*
                 * Randomized delay for multicast suppression.
                 *
                 * 1 to 5 seconds is enough for the first responder to be heard
                 * by the other peers before their timers fire.
                 */
                uint64_t delay = 1 + (uint64_t)(rand() % 5);
                g->roster_reply_deadline = now + delay;

                printf("[MID] ROSTER_REQUEST: scheduled roster reply in %llu seconds. Our FP[0..3]=%02X%02X%02X%02X\n",
                       (unsigned long long)delay,
                       g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]);
                fflush(stdout);
            }
        }

        return changed;
    }

    /* Unknown message type. */
    return false;
}

/******************************************************************************
Forward declaration for internal use.
mid_delete_peer_by_identity_internal is defined later in this file but is
needed by mid_on_group_moderation_internal. We must NOT call the public
mid_delete_peer_by_identity() from within a locked section because it would
attempt to re-acquire the non-recursive mutex → deadlock.
******************************************************************************/
static bool mid_delete_peer_by_identity_internal(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                                                 const uint8_t identity_key[MID_IDENTITY_KEY_SIZE]);

/******************************************************************************
Internal state management (Assumes lock is held, does NOT fire callbacks)

THREAD-SAFETY CONTRACT:
Every function in this section expects the caller to already hold s->mutex.
These functions must ONLY call other "_internal" or static helpers.
They must NEVER call the public (locking) API wrappers.
******************************************************************************/

static bool mid_on_group_self_join_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const uint8_t *nickname, size_t nickname_len)
{
    MidGroupState *g = mid_find_group(s, chat_id);
    bool is_new = false;

    if (!g) {
        g = mid_add_group(s, chat_id);
        is_new = true;
    }

    if (!g || !mid_init_self_from_tox(g, tox)) return is_new;

    /* Stamp the founder role as soon as we join */
    mid_apply_founder_role(g, tox);

    bool nick_changed = false;

    /*
     * Store the self nickname as raw bytes.
     */
    if (nickname != NULL && nickname_len > 0) {
        if (nickname_len > MID_MAX_NICK_SIZE) {
            nickname_len = MID_MAX_NICK_SIZE;
        }
        if (g->self_nickname_len != nickname_len || memcmp(g->self_nickname, nickname, nickname_len) != 0) {
            memset(g->self_nickname, 0, MID_MAX_NICK_SIZE);
            memcpy(g->self_nickname, nickname, nickname_len);
            g->self_nickname_len = (uint16_t)nickname_len;
            nick_changed = true;
        }
    } else {
        if (g->self_nickname_len != 0) {
            memset(g->self_nickname, 0, MID_MAX_NICK_SIZE);
            g->self_nickname_len = 0;
            nick_changed = true;
        }
    }

    g->announced = true;

    bool announce_changed = mid_announce_self_group(s, g, tox);
    mid_send_roster_request_group(s, g, tox);

    return is_new || nick_changed || announce_changed;
}

static bool mid_on_group_delete_internal(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return false;

    for (size_t i = 0; i < s->group_count; i++) {
        if (memcmp(s->groups[i].chat_id, chat_id, TOX_GROUP_CHAT_ID_SIZE) == 0) {
            sodium_memzero(s->groups[i].self_secret_signing_key, sizeof(s->groups[i].self_secret_signing_key));
            free(s->groups[i].records);
            for (size_t j = i; j < s->group_count - 1; j++) {
                s->groups[j] = s->groups[j+1];
            }
            s->group_count--;
            return true; /* CHANGED */
        }
    }
    return false;
}

static bool mid_on_group_peer_join_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], uint32_t peer_id)
{
    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g) return false;

    bool changed = false;

    uint32_t group_number = mid_chat_id_to_group_number(tox, chat_id);
    if (group_number == UINT32_MAX) return false;

    uint8_t id[MID_IDENTITY_KEY_SIZE];
    uint8_t sig[MID_SIGNING_KEY_SIZE];

    Tox_Err_Group_Peer_Query conn_err;
    Tox_Connection conn = tox_group_peer_get_connection_status(tox, group_number, peer_id, &conn_err);
    if (conn_err != TOX_ERR_GROUP_PEER_QUERY_OK) conn = TOX_CONNECTION_NONE;

    Tox_Err_Group_Peer_Query role_err;
    Tox_Group_Role role = tox_group_peer_get_role(tox, group_number, peer_id, &role_err);
    if (role_err != TOX_ERR_GROUP_PEER_QUERY_OK) role = TOX_GROUP_ROLE_USER;

    if (mid_get_peer_keys(tox, chat_id, peer_id, id, sig)) {
        if (mid_on_peer_online_keys(g, id, sig, conn, role, mid_now_or_time(0))) {
            changed = true;
        }
    }

    if (mid_refresh_peer_name_group(g, tox, peer_id)) {
        changed = true;
    }

    if (g->announced) {
        if (mid_announce_self_group(s, g, tox)) {
            changed = true;
        }
        
        /* 
         * RELIABILITY FIX:
         * Toxcore often drops custom packets sent immediately during the join handshake.
         * The new peer's ROSTER_REQUEST might be lost (as seen in the logs).
         * Instead, when existing peers see a new peer join, they schedule a delayed 
         * ROSTER_BATCH broadcast. The fingerprint suppression ensures only ONE existing 
         * peer actually sends it, preventing a broadcast storm while guaranteeing delivery.
         */
        if (g->roster_reply_deadline == 0) {
            uint64_t delay = 1 + (uint64_t)(rand() % 5);
            g->roster_reply_deadline = mid_now_or_time(0) + delay;
            printf("[MID] peer_join: scheduled roster sync in %llu seconds because a new peer joined.\n", (unsigned long long)delay);
            fflush(stdout);
            changed = true;
        }
    }

    return changed;
}

static bool mid_on_group_peer_exit_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], Tox_Group_Exit_Type exit_type)
{
    (void)exit_type; /* Unused currently */

    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g) return false;

    return mid_sync_online_state_group(g, tox, mid_now_or_time(0));
}

static bool mid_on_group_moderation_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], uint32_t source_peer_id, uint32_t target_peer_id, Tox_Group_Mod_Event mod_type, bool *out_changed)
{
    *out_changed = false;

    if (s == NULL || tox == NULL || chat_id == NULL) return false;

    MidGroupState *g = mid_find_group(s, chat_id);
    if (g == NULL) return false;

    uint32_t group_number = mid_chat_id_to_group_number(tox, chat_id);
    if (group_number == UINT32_MAX) return false;

    printf("[MID] on_group_moderation: event group=%u source_peer=%u target_peer=%u type=%d\n",
           group_number, source_peer_id, target_peer_id, (int)mod_type);
    fflush(stdout);

    if (mod_type == TOX_GROUP_MOD_EVENT_KICK) {
        Tox_Err_Group_Self_Query self_err;
        uint32_t self_peer_id = tox_group_self_get_peer_id(tox, group_number, &self_err);

        if (self_err == TOX_ERR_GROUP_SELF_QUERY_OK && target_peer_id == self_peer_id) {
            printf("[MID] on_group_moderation: we were kicked; deleting group state\n");
            fflush(stdout);
            mid_on_group_delete_internal(s, chat_id);
            *out_changed = true;
            return true;
        }

        Tox_Err_Group_Peer_Query peer_err;
        uint8_t kicked_identity[MID_IDENTITY_KEY_SIZE];

        if (tox_group_peer_get_public_key(tox, group_number, target_peer_id, kicked_identity, &peer_err) &&
            peer_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
            printf("[MID] on_group_moderation: deleting kicked peer from roster\n");
            fflush(stdout);
            /*
             * THREAD-SAFETY FIX:
             * We are already holding s->mutex (acquired by the public caller).
             * We must call the _internal variant which does NOT lock.
             * Calling the public mid_delete_peer_by_identity() here would
             * deadlock on the non-recursive mutex.
             */
            if (mid_delete_peer_by_identity_internal(s, chat_id, kicked_identity)) {
                *out_changed = true;
            }
        } else {
            printf("[MID] on_group_moderation: kicked peer %u identity no longer queryable\n", target_peer_id);
            fflush(stdout);
        }
    } else if (mod_type == TOX_GROUP_MOD_EVENT_OBSERVER ||
               mod_type == TOX_GROUP_MOD_EVENT_USER ||
               mod_type == TOX_GROUP_MOD_EVENT_MODERATOR) {
        
        Tox_Err_Group_Peer_Query peer_err;
        uint8_t target_identity[MID_IDENTITY_KEY_SIZE];

        if (tox_group_peer_get_public_key(tox, group_number, target_peer_id, target_identity, &peer_err) &&
            peer_err == TOX_ERR_GROUP_PEER_QUERY_OK) {
            
            int idx = mid_find_identity(g, target_identity);
            if (idx >= 0) {
                Tox_Group_Role new_role = TOX_GROUP_ROLE_USER;
                if (mod_type == TOX_GROUP_MOD_EVENT_OBSERVER) new_role = TOX_GROUP_ROLE_OBSERVER;
                else if (mod_type == TOX_GROUP_MOD_EVENT_MODERATOR) new_role = TOX_GROUP_ROLE_MODERATOR;
                
                if (g->records[idx].role != new_role) {
                    g->records[idx].role = new_role;
                    *out_changed = true;
                }
            }
        }
    }

    return false;
}

static bool mid_on_group_peer_name_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], uint32_t peer_id)
{
    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g) return false;

    return mid_refresh_peer_name_group(g, tox, peer_id);
}

static bool mid_self_set_name_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const uint8_t *nickname, size_t nickname_len)
{
    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g || !g->have_keys) return false;

    bool nick_changed = false;

    /* Update the persistent self nickname buffer */
    if (nickname != NULL && nickname_len > 0) {
        if (nickname_len > MID_MAX_NICK_SIZE) {
            nickname_len = MID_MAX_NICK_SIZE;
        }
        if (g->self_nickname_len != nickname_len || memcmp(g->self_nickname, nickname, nickname_len) != 0) {
            memset(g->self_nickname, 0, MID_MAX_NICK_SIZE);
            memcpy(g->self_nickname, nickname, nickname_len);
            g->self_nickname_len = (uint16_t)nickname_len;
            nick_changed = true;
        }
    } else {
        if (g->self_nickname_len != 0) {
            memset(g->self_nickname, 0, MID_MAX_NICK_SIZE);
            g->self_nickname_len = 0;
            nick_changed = true;
        }
    }

    if (nick_changed) {
        /* Immediately update the local roster record so it reflects the change
           even if the timestamp doesn't bump within the same second */
        int idx = mid_find_identity(g, g->self_identity_key);
        if (idx >= 0) {
            MidPeerRecord *e = &g->records[idx];
            memset(e->nickname, 0, MID_MAX_NICK_SIZE);
            if (g->self_nickname_len > 0) {
                memcpy(e->nickname, g->self_nickname, g->self_nickname_len);
            }
            e->nickname_len = g->self_nickname_len;
        }

        /* Re-announce ourselves to broadcast the new signed presence record */
        if (g->announced) {
            mid_announce_self_group(s, g, tox);
        }
    }

    return nick_changed;
}

static bool mid_on_group_custom_packet_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], uint32_t peer_id, const uint8_t *data, size_t length)
{
    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g) return false;

    s->recv_bytes += length;

    return mid_on_custom_packet_group(s, g, tox, peer_id, data, length, mid_now_or_time(0));
}

static bool mid_announce_leave_internal(MidState *s, Tox *tox, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], bool *out_changed)
{
    *out_changed = false;

    if (!chat_id) return false;

    MidGroupState *g = mid_find_group(s, chat_id);
    if (!g || !g->have_keys) return false;

    MidPeerRecord r;
    memset(&r, 0, sizeof(r));
    memcpy(r.identity_key, g->self_identity_key, MID_IDENTITY_KEY_SIZE);
    memcpy(r.signing_key, g->self_signing_key, MID_SIGNING_KEY_SIZE);
    r.status = MID_STATUS_LEFT;
    r.timestamp = mid_now_or_time(0);
    r.connection_status = TOX_CONNECTION_NONE;

    if (g->self_nickname_len > 0) {
        uint16_t nick_copy = g->self_nickname_len;
        if (nick_copy > MID_MAX_NICK_SIZE) {
            nick_copy = MID_MAX_NICK_SIZE;
        }
        memcpy(r.nickname, g->self_nickname, nick_copy);
        r.nickname_len = nick_copy;
    }

    if (!mid_sign_record(g, &r)) return false;

    printf("[MID] announce_leave: broadcasting signed LEFT tombstone\n"); fflush(stdout);

    bool sent = mid_send_presence_record(s, g, tox, &r);

    int idx = mid_find_identity(g, g->self_identity_key);
    if (idx >= 0) {
        /* Replacing self with tombstone */
        g->records[idx] = r;
        g->records[idx].connection_status = TOX_CONNECTION_NONE;
        *out_changed = true;
    }

    g->self_active = false;
    return sent;
}

/*
Internal delete: assumes lock is already held. Does NOT fire callbacks.
This is the non-locking counterpart of the public mid_delete_peer_by_identity().
*/
static bool mid_delete_peer_by_identity_internal(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE],
                                                 const uint8_t identity_key[MID_IDENTITY_KEY_SIZE])
{
    if (s == NULL || identity_key == NULL || chat_id == NULL) return false;
    if (mid_key_is_zero(identity_key)) return false;

    MidGroupState *g = mid_find_group(s, chat_id);
    if (g == NULL) return false;

    int idx = mid_find_identity(g, identity_key);
    if (idx < 0) return false;

    printf("[MID] delete_peer_by_identity: deleting peer from roster\n");
    fflush(stdout);

    if (g->records[idx].has_signature) {
        printf("[MID] delete_peer: XOR OUT deleted peer. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
        mid_xor_fingerprint(g->roster_fingerprint, g->records[idx].identity_key);
        printf("[MID] delete_peer: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
    }

    for (size_t k = (size_t)idx; k + 1 < g->count; k++) {
        g->records[k] = g->records[k+1];
    }
    g->count--;
    return true; /* CHANGED */
}

/******************************************************************************
Public state management (Handles Locking and Callbacks)

THREAD-SAFETY:
Every public entry point follows the same pattern:
  1. mid_lock(s)
  2. Call the corresponding _internal function (which assumes lock is held)
  3. Snapshot the callback pointer under the lock
  4. mid_unlock(s)
  5. Fire the callback OUTSIDE the lock (prevents deadlock if the
     callback calls back into any mid_* public function)
******************************************************************************/

static bool mid_load_from_disk(MidState *s, const char *path, const uint8_t *passphrase, size_t passphrase_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { fclose(f); return false; }

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data || fread(file_data, 1, file_size, f) != (size_t)file_size) {
        free(file_data); fclose(f); return false;
    }
    fclose(f);

    uint8_t *data_to_parse = file_data;
    size_t data_len = file_size;
    uint8_t *decrypted_data = NULL;

    // Check if the file was encrypted by TOXENCRYPTSAVE
    if (file_size >= TOX_PASS_ENCRYPTION_EXTRA_LENGTH && tox_is_data_encrypted(file_data)) {
        if (!passphrase || passphrase_len == 0) {
            printf("[MID] load: file is encrypted but no passphrase provided, ignoring\n");
            free(file_data);
            return false;
        }
        decrypted_data = (uint8_t *)malloc(file_size);
        if (!decrypted_data) {
            free(file_data);
            return false;
        }
        Tox_Err_Decryption err;
        if (!tox_pass_decrypt(file_data, file_size, passphrase, passphrase_len, decrypted_data, &err)) {
            printf("[MID] load: decryption failed (err=%d)\n", err);
            free(decrypted_data);
            free(file_data);
            return false;
        }
        data_to_parse = decrypted_data;
        data_len = file_size - TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
    }

    // Parse Header
    MidReader r = { data_to_parse, data_len, 0 };
    uint8_t magic[MID_SAVE_MAGIC_SIZE];
    if (!mid_reader_read(&r, magic, MID_SAVE_MAGIC_SIZE) || memcmp(magic, MID_SAVE_MAGIC, MID_SAVE_MAGIC_SIZE) != 0) {
        goto parse_fail;
    }

    uint32_t ver;
    if (!mid_reader_read_u32(&r, &ver) || ver != MID_SAVE_VERSION) {
        printf("[MID] load: unsupported or missing version %u\n", ver);
        goto parse_fail;
    }

    uint64_t load_time = (uint64_t)time(NULL);

    // Parse Body
    uint32_t group_count;
    if (!mid_reader_read_u32(&r, &group_count) || group_count > MID_MAX_GROUPS) goto parse_fail;

    for (uint32_t i = 0; i < group_count; i++) {
        uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
        if (!mid_reader_read(&r, chat_id, TOX_GROUP_CHAT_ID_SIZE)) goto parse_fail;

        MidGroupState *g = mid_add_group(s, chat_id);
        if (!g) goto parse_fail;

        if (!mid_reader_read(&r, g->self_identity_key, MID_IDENTITY_KEY_SIZE)) goto parse_fail;
        if (!mid_reader_read(&r, g->self_signing_key, MID_SIGNING_KEY_SIZE)) goto parse_fail;
        /*
         * NEVER load self_secret_signing_key from disk.
         *
         * The secret key is fetched from Toxcore once the group becomes
         * active again via mid_init_self_from_tox().
         */
        sodium_memzero(g->self_secret_signing_key, sizeof(g->self_secret_signing_key));

        /*
         * We only have public keys at this point.
         * We cannot sign anything until Toxcore gives us the secret signing key.
         */
        g->have_keys = false;
        g->self_active = false;
        g->announced = false;

        uint16_t nick_len;
        if (!mid_reader_read_u16(&r, &nick_len) || nick_len > MID_MAX_NICK_SIZE) goto parse_fail;
        g->self_nickname_len = nick_len;
        if (!mid_reader_read(&r, g->self_nickname, nick_len)) goto parse_fail;

        uint32_t peer_count;
        if (!mid_reader_read_u32(&r, &peer_count) || peer_count > MID_MAX_PEERS_PER_GROUP) goto parse_fail;

        for (uint32_t j = 0; j < peer_count; j++) {
            MidPeerRecord tmp_rec;
            memset(&tmp_rec, 0, sizeof(tmp_rec));

            if (!mid_reader_read(&r, tmp_rec.identity_key, MID_IDENTITY_KEY_SIZE)) goto parse_fail;
            if (!mid_reader_read(&r, tmp_rec.signing_key, MID_SIGNING_KEY_SIZE)) goto parse_fail;

            uint8_t status;
            if (!mid_reader_read_u8(&r, &status)) goto parse_fail;
            tmp_rec.status = status;
            if (tmp_rec.status != MID_STATUS_ACTIVE && tmp_rec.status != MID_STATUS_LEFT) goto parse_fail;

            if (!mid_reader_read_u64(&r, &tmp_rec.timestamp)) goto parse_fail;

            uint16_t p_nick_len;
            if (!mid_reader_read_u16(&r, &p_nick_len) || p_nick_len > MID_MAX_NICK_SIZE) goto parse_fail;
            tmp_rec.nickname_len = p_nick_len;
            if (!mid_reader_read(&r, tmp_rec.nickname, p_nick_len)) goto parse_fail;

            if (!mid_reader_read(&r, tmp_rec.signature, MID_SIG_SIZE)) goto parse_fail;
            uint8_t has_sig;
            if (!mid_reader_read_u8(&r, &has_sig)) goto parse_fail;
            tmp_rec.has_signature = has_sig ? true : false;

            uint32_t conn, role;
            if (!mid_reader_read_u32(&r, &conn)) goto parse_fail;
            /* HINT: Force all loaded peers to offline.
                     Leaves status (ACTIVE/LEFT) exactly as it was saved. */
            // tmp_rec.connection_status = (Tox_Connection)conn;
            tmp_rec.connection_status = TOX_CONNECTION_NONE;

            if (!mid_reader_read_u32(&r, &role)) goto parse_fail;
            tmp_rec.role = (Tox_Group_Role)role;


            if (!mid_reader_read_u64(&r, &tmp_rec.last_seen)) goto parse_fail;
            // Only anchor if the saved value was 0 (e.g. corrupted or legacy save).
            // If you use this, and the app was closed for 31 days, offline peers
            // WILL be purged immediately upon the first mid_iterate() call.
            //
            if (tmp_rec.last_seen == 0) {
                tmp_rec.last_seen = load_time;
            }

            if (mid_find_identity(g, tmp_rec.identity_key) >= 0) continue;

            if (!mid_ensure_capacity(g)) goto parse_fail;
            MidPeerRecord *p = &g->records[g->count];
            *p = tmp_rec;

            if (p->has_signature) {
                mid_xor_fingerprint(g->roster_fingerprint, p->identity_key);
            }
            g->count++;
        }
    }

    if (decrypted_data) free(decrypted_data);
    free(file_data);
    return true;

parse_fail:
    printf("[MID] load: parse error, discarding loaded state\n");
    if (decrypted_data) free(decrypted_data);
    free(file_data);
    for (size_t i = 0; i < s->group_count; i++) free(s->groups[i].records);
    free(s->groups);
    s->groups = NULL; s->group_count = 0; s->group_capacity = 0;
    return false;
}

bool mid_save(MidState *s, const uint8_t *passphrase, size_t passphrase_len) {
    if (!s || !s->save_path) return false;

    mid_lock(s);
    MidBuf buf;
    mid_buf_init(&buf);

    // 1. Assemble Header and Body
    mid_buf_append(&buf, MID_SAVE_MAGIC, MID_SAVE_MAGIC_SIZE);
    uint8_t ver[4]; 
    mid_put_u32_be(ver, MID_SAVE_VERSION); 
    mid_buf_append(&buf, ver, sizeof(ver));

    // 2. Serialize Body
    mid_buf_append_u32(&buf, (uint32_t)s->group_count);
    for (size_t i = 0; i < s->group_count; i++) {
        MidGroupState *g = &s->groups[i];
        mid_buf_append(&buf, g->chat_id, TOX_GROUP_CHAT_ID_SIZE);
        mid_buf_append(&buf, g->self_identity_key, MID_IDENTITY_KEY_SIZE);
        mid_buf_append(&buf, g->self_signing_key, MID_SIGNING_KEY_SIZE);
        /*
         * NEVER save self_secret_signing_key.
         *
         * The Ed25519 signing secret key belongs to the NGC group identity
         * and is owned by Toxcore. It must be re-obtained from Toxcore when
         * the group is active again.
         */
        mid_buf_append_u16(&buf, g->self_nickname_len);
        mid_buf_append(&buf, g->self_nickname, g->self_nickname_len);
        mid_buf_append_u32(&buf, (uint32_t)g->count);

        for (size_t j = 0; j < g->count; j++) {
            MidPeerRecord *p = &g->records[j];
            mid_buf_append(&buf, p->identity_key, MID_IDENTITY_KEY_SIZE);
            mid_buf_append(&buf, p->signing_key, MID_SIGNING_KEY_SIZE);
            mid_buf_append_u8(&buf, p->status);
            mid_buf_append_u64(&buf, p->timestamp);
            mid_buf_append_u16(&buf, p->nickname_len);
            mid_buf_append(&buf, p->nickname, p->nickname_len);
            mid_buf_append(&buf, p->signature, MID_SIG_SIZE);
            mid_buf_append_u8(&buf, p->has_signature ? 1 : 0);
            mid_buf_append_u32(&buf, (uint32_t)p->connection_status);
            mid_buf_append_u32(&buf, (uint32_t)p->role);
            mid_buf_append_u64(&buf, p->last_seen);
        }
    }

    // 3. Encrypt payload if passphrase provided
    uint8_t *final_payload = buf.data;
    size_t final_payload_len = buf.len;
    uint8_t *free_payload = NULL;

    if (passphrase && passphrase_len > 0) {
        size_t cipher_len = buf.len + TOX_PASS_ENCRYPTION_EXTRA_LENGTH;
        free_payload = (uint8_t *)malloc(cipher_len);
        if (!free_payload) { mid_buf_free(&buf); mid_unlock(s); return false; }

        Tox_Err_Encryption err;
        if (!tox_pass_encrypt(buf.data, buf.len, passphrase, passphrase_len, free_payload, &err)) {
            printf("[MID] save: encryption failed (err=%d)\n", err);
            free(free_payload); mid_buf_free(&buf); mid_unlock(s); return false;
        }
        final_payload = free_payload;
        final_payload_len = cipher_len;
    }

    char *path_copy = mid_strdup(s->save_path);
    mid_unlock(s); // <--- CRITICAL: Release lock before doing blocking Disk I/O

    if (!path_copy) {
        if (free_payload) free(free_payload);
        mid_buf_free(&buf);
        return false;
    }

    // 4. Write to Disk Atomically
    size_t path_len = strlen(path_copy);
    char *tmp_path = (char *)malloc(path_len + 6);
    if (!tmp_path) {
        free(path_copy);
        if (free_payload) free(free_payload);
        mid_buf_free(&buf);
        return false;
    }
    snprintf(tmp_path, path_len + 6, "%s.tmp", path_copy);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(tmp_path); free(path_copy);
        if (free_payload) free(free_payload);
        mid_buf_free(&buf);
        return false;
    }

#ifndef _WIN32
    /* 
     * FIX: CodeQL TOCTOU Race Condition.
     * Instead of closing the file and calling chmod() on the path string 
     * (which allows an attacker to swap the path with a symlink in between),
     * we use fchmod() on the open file descriptor. This atomically secures 
     * the exact file we just created, completely bypassing path-based races.
     */
    if (fchmod(fileno(f), 0600) != 0) {
        // Silently ignore permission errors, matching previous behavior
    }
#endif

    fwrite(final_payload, 1, final_payload_len, f);
    fclose(f);

    if (free_payload) free(free_payload);
    mid_buf_free(&buf);

#ifdef _WIN32
    /*
     * Windows filesystem semantics differ from POSIX. Symlink-based TOCTOU 
     * attacks via _chmod are not part of the standard Windows threat model 
     * in this context, so _chmod remains the correct and standard approach here.
     */
    _chmod(tmp_path, _S_IREAD | _S_IWRITE);
#endif

#ifdef __MINGW32__
    // HINT: rename() will refuse to overwrite existing files with WIN32 mingw
    unlink(path_copy);
#endif

    bool ok = (rename(tmp_path, path_copy) == 0);
    if (!ok) remove(tmp_path);

    free(tmp_path);
    free(path_copy);
    return ok;
}

MidState *mid_new(const char *save_path, const uint8_t *passphrase, size_t passphrase_len) {
    if (sodium_init() < 0) return NULL;

    MidState *s = (MidState *)calloc(1, sizeof(MidState));
    if (!s) return NULL;

    s->mutex = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    if (!s->mutex) { free(s); return NULL; }
    pthread_mutex_init(s->mutex, NULL);

    if (save_path) s->save_path = mid_strdup(save_path);

    if (s->save_path) {
        mid_load_from_disk(s, s->save_path, passphrase, passphrase_len);
    }

    return s;
}

void mid_free(MidState *s) {
    if (s == NULL) return;

    for (size_t i = 0; i < s->group_count; i++) {
        sodium_memzero(s->groups[i].self_secret_signing_key, sizeof(s->groups[i].self_secret_signing_key));
        free(s->groups[i].records);
    }
    free(s->groups);

    free(s->save_path);

    if (s->mutex) {
        pthread_mutex_destroy(s->mutex);
        free(s->mutex);
    }

    free(s);
}

void mid_on_group_self_join(MidState *s, Tox *tox, uint32_t group_number, const uint8_t *nickname, size_t nickname_len)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_self_join_internal(s, tox, chat_id, nickname, nickname_len);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    /* Fire callback OUTSIDE the lock to avoid deadlock */
    if (changed && cb) cb(chat_id, ud);
}

void mid_on_group_delete(MidState *s, Tox *tox, uint32_t group_number)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_delete_internal(s, chat_id);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
}

void mid_on_group_peer_join(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_peer_join_internal(s, tox, chat_id, peer_id);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
}

void mid_on_group_peer_exit(MidState *s, Tox *tox, uint32_t group_number, Tox_Group_Exit_Type exit_type)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_peer_exit_internal(s, tox, chat_id, exit_type);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
}

bool mid_on_group_moderation(MidState *s, Tox *tox, uint32_t group_number,
                             uint32_t source_peer_id, uint32_t target_peer_id,
                             Tox_Group_Mod_Event mod_type)
{
    if (!s || !tox) return false;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return false;

    mid_lock(s);
    bool changed = false;
    bool we_were_kicked = mid_on_group_moderation_internal(s, tox, chat_id, source_peer_id, target_peer_id, mod_type, &changed);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
    return we_were_kicked;
}

void mid_on_group_peer_name(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_peer_name_internal(s, tox, chat_id, peer_id);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
}

bool mid_self_set_name(MidState *s, Tox *tox, uint32_t group_number, const uint8_t *nickname, size_t nickname_len)
{
    if (!s || !tox) return false;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return false;

    mid_lock(s);
    bool changed = mid_self_set_name_internal(s, tox, chat_id, nickname, nickname_len);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    /* Fire callback OUTSIDE the lock to avoid deadlock */
    if (changed && cb) cb(chat_id, ud);

    return changed;
}

void mid_on_group_custom_packet(MidState *s, Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *data, size_t length)
{
    if (!s || !tox) return;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return;

    mid_lock(s);
    bool changed = mid_on_group_custom_packet_internal(s, tox, chat_id, peer_id, data, length);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
}

bool mid_announce_leave(MidState *s, Tox *tox, uint32_t group_number)
{
    if (!s || !tox) return false;

    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries err;
    if (!tox_group_get_chat_id(tox, group_number, chat_id, &err) || err != TOX_ERR_GROUP_STATE_QUERIES_OK) return false;

    mid_lock(s);
    bool changed = false;
    bool sent = mid_announce_leave_internal(s, tox, chat_id, &changed);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
    return sent;
}

void mid_iterate(MidState *s, Tox *tox)
{
    if (!s || !tox) return;

    mid_lock(s);

    uint64_t now = mid_now_or_time(0);

    /*
     * PERFORMANCE THROTTLE:
     * Do not run the heavy sync/cleanup loop on every 50ms tox_iterate() tick.
     * Only run it every MID_SYNC_INTERVAL_SEC seconds.
     */
    if (now - s->last_sync_time < MID_SYNC_INTERVAL_SEC) {
        mid_unlock(s);
        return;
    }

    s->last_sync_time = now;

    /* Collect changed groups to fire callbacks AFTER unlocking */
    uint8_t changed_groups[MID_MAX_CHANGED_GROUPS_PER_ITERATE][TOX_GROUP_CHAT_ID_SIZE];
    size_t num_changed = 0;

    for (size_t i = 0; i < s->group_count; i++) {
        MidGroupState *g = &s->groups[i];
        bool changed = false;

        // 1. Sync online state with Toxcore
        if (mid_sync_online_state_group(g, tox, now)) {
            changed = true;
        }

        // 2. Heartbeats (FIX 3: Uses lightweight heartbeat packet)
        if (g->have_keys && g->self_active && g->announced) {
            if (now >= g->last_announce && (now - g->last_announce >= MID_HEARTBEAT_SEC)) {
                printf("[MID] poll: heartbeat triggered\n"); fflush(stdout);
                if (mid_send_heartbeat_record(s, g, tox)) {
                    g->last_announce = now;
                    int idx = mid_find_identity(g, g->self_identity_key);
                    if (idx >= 0) {
                        g->records[idx].timestamp = now;
                    }
                    changed = true;
                }
            }
        }

        // 3. GRAVEYARD & STALE CLEANUP
        uint64_t cutoff_tombstone = (now > MID_TOMBSTONE_TTL_SEC) ? (now - MID_TOMBSTONE_TTL_SEC) : 0;
        uint64_t cutoff_stale = (now > MID_STALE_PEER_TTL_SEC) ? (now - MID_STALE_PEER_TTL_SEC) : 0;

        size_t j = 0;
        while (j < g->count) {
            bool purge = false;
            const MidPeerRecord *rec = &g->records[j];

            // A. Purge expired LEFT tombstones (7 days)
            if (rec->status == MID_STATUS_LEFT && rec->timestamp < cutoff_tombstone) {
                purge = true;
            }
            // B. Purge stale offline ACTIVE peers (30 days)
            // Must be ACTIVE, offline, and last_seen is older than the stale cutoff.
            else if (rec->status == MID_STATUS_ACTIVE &&
                     rec->connection_status == TOX_CONNECTION_NONE &&
                     rec->last_seen > 0 &&
                     rec->last_seen < cutoff_stale) {
                purge = true;
            }

            if (purge) {
                printf("[MID] iterate: purging stale/expired peer %zu\n", j); fflush(stdout);
                if (rec->has_signature) {
                    printf("[MID] iterate: XOR OUT purged peer. Old FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
                    mid_xor_fingerprint(g->roster_fingerprint, rec->identity_key);
                    printf("[MID] iterate: New FP[0..3]=%02X%02X%02X%02X\n", g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]); fflush(stdout);
                }
                // Shift array down to delete
                for (size_t k = j; k < g->count - 1; k++) {
                    g->records[k] = g->records[k+1];
                }
                g->count--;
                changed = true;
                continue; // Do not increment j, check the new record at this index
            }
            j++;
        }

        // 4. FIX 2: Multicast Roster Reply Timer execution
        if (g->roster_reply_deadline != 0 && now >= g->roster_reply_deadline) {
            printf("[MID] iterate: Roster reply timer fired! No one else suppressed us. Sending our roster. Our FP[0..3]=%02X%02X%02X%02X\n",
                   g->roster_fingerprint[0], g->roster_fingerprint[1], g->roster_fingerprint[2], g->roster_fingerprint[3]);
            fflush(stdout);
            g->roster_reply_deadline = 0;
            mid_send_roster_group(s, g, tox);
            changed = true;
        }

        if (changed && num_changed < MID_MAX_CHANGED_GROUPS_PER_ITERATE) {
            memcpy(changed_groups[num_changed++], g->chat_id, TOX_GROUP_CHAT_ID_SIZE);
        }
    }

    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;

    /*
     * IMPORTANT: Unlock BEFORE firing callbacks.
     * This prevents deadlocks if the user's callback calls mid_peer_list_get().
     */
    mid_unlock(s);

    if (cb) {
        for (size_t i = 0; i < num_changed; i++) {
            cb(changed_groups[i], ud);
        }
    }
}

size_t mid_group_count(MidState *s)
{
    if (!s) return 0;

    mid_lock(s);
    size_t count = s->group_count;
    mid_unlock(s);

    return count;
}

int mid_find_peer(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const uint8_t identity_key[MID_IDENTITY_KEY_SIZE])
{
    if (s == NULL || identity_key == NULL || chat_id == NULL) return -1;
    if (mid_key_is_zero(identity_key)) return -1;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    int idx = -1;
    if (g != NULL) {
        idx = mid_find_identity(g, identity_key);
    }
    mid_unlock(s);

    return idx;
}

bool mid_delete_peer_by_identity(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const uint8_t identity_key[MID_IDENTITY_KEY_SIZE])
{
    if (!s || !chat_id) return false;

    mid_lock(s);
    bool changed = mid_delete_peer_by_identity_internal(s, chat_id, identity_key);
    mid_peer_list_changed_cb cb = s->peer_list_changed_cb;
    void *ud = s->peer_list_changed_user_data;
    mid_unlock(s);

    if (changed && cb) cb(chat_id, ud);
    return changed;
}

size_t mid_peer_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return 0;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    size_t count = g ? g->count : 0;
    mid_unlock(s);

    return count;
}

size_t mid_signed_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return 0;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    size_t c = 0;
    if (g) {
        for (size_t i = 0; i < g->count; i++) {
            if (g->records[i].has_signature) c++;
        }
    }
    mid_unlock(s);

    return c;
}

size_t mid_online_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return 0;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    size_t c = 0;
    if (g) {
        for (size_t i = 0; i < g->count; i++) {
            if (g->records[i].connection_status != TOX_CONNECTION_NONE) c++;
        }
    }
    mid_unlock(s);

    return c;
}

size_t mid_offline_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return 0;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    size_t c = 0;
    if (g) {
        for (size_t i = 0; i < g->count; i++) {
            if (g->records[i].connection_status == TOX_CONNECTION_NONE &&
                g->records[i].status == MID_STATUS_ACTIVE) c++;
        }
    }
    mid_unlock(s);

    return c;
}

bool mid_peer_is_signed_left(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const uint8_t identity_key[MID_IDENTITY_KEY_SIZE])
{
    if (s == NULL || identity_key == NULL || chat_id == NULL) return false;
    if (mid_key_is_zero(identity_key)) return false;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    bool res = false;
    if (g != NULL) {
        int idx = mid_find_identity(g, identity_key);
        if (idx >= 0) {
            const MidPeerRecord *rec = &g->records[idx];
            res = rec->has_signature && rec->status == MID_STATUS_LEFT;
        }
    }
    mid_unlock(s);

    return res;
}

/******************************************************************************
Network Stats
******************************************************************************/

void mid_get_network_stats(MidState *s, uint64_t *sent_bytes, uint64_t *recv_bytes)
{
    if (s == NULL) {
        if (sent_bytes) *sent_bytes = 0;
        if (recv_bytes) *recv_bytes = 0;
        return;
    }

    /* Cast away const for locking; the mutex is logically mutable */
    mid_lock(s);
    if (sent_bytes) *sent_bytes = s->sent_bytes;
    if (recv_bytes) *recv_bytes = s->recv_bytes;
    mid_unlock(s);
}

/******************************************************************************
Peer List Query API (for C / JNI clients)
******************************************************************************/

size_t mid_peer_list_count(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE])
{
    if (!s || !chat_id) return 0;

    mid_lock(s);
    const MidGroupState *g = mid_find_group_const(s, chat_id);
    size_t count = g ? g->count : 0;
    mid_unlock(s);

    return count;
}

bool mid_peer_list_get(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], size_t index, MidPeerInfo *out)
{
    if (s == NULL || out == NULL || chat_id == NULL) return false;

    mid_lock(s);

    const MidGroupState *g = mid_find_group_const(s, chat_id);
    bool success = false;

    if (g != NULL && index < g->count) {
        const MidPeerRecord *rec = &g->records[index];

        memset(out, 0, sizeof(*out));

        memcpy(out->identity_key, rec->identity_key, MID_IDENTITY_KEY_SIZE);
        memcpy(out->signing_key, rec->signing_key, MID_SIGNING_KEY_SIZE);

        out->status        = rec->status;
        out->connection_status = (uint8_t)rec->connection_status;
        out->role          = (uint8_t)rec->role;
        out->has_signature = rec->has_signature ? 1 : 0;
        out->reserved      = 0;
        out->last_seen     = rec->last_seen;
        out->nickname_len  = rec->nickname_len;

        // Safe copy bounded strictly by MID_MAX_NICK_SIZE
        if (rec->nickname_len > 0 && rec->nickname_len <= MID_MAX_NICK_SIZE) {
            memcpy(out->nickname, rec->nickname, rec->nickname_len);
        }

        // The NUL-termination.
        // Because the array in MidPeerInfo is (MID_MAX_NICK_SIZE + 1),
        // the index [nickname_len] is ALWAYS a valid memory location.
        // (Max index written to is MID_MAX_NICK_SIZE, which is the last byte).
        out->nickname[out->nickname_len] = '\0';

        success = true;
    }

    mid_unlock(s);

    return success;
}

/******************************************************************************
Peer List Changed Callback registration
******************************************************************************/

void mid_set_peer_list_changed_cb(MidState *s, mid_peer_list_changed_cb cb, void *user_data)
{
    if (!s) return;

    mid_lock(s);
    s->peer_list_changed_cb = cb;
    s->peer_list_changed_user_data = user_data;
    mid_unlock(s);
}

/******************************************************************************
Peer table printing
******************************************************************************/

static void mid_hex_short(char *out, size_t out_size, const uint8_t *data, size_t data_len, size_t max_bytes)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (data == NULL || data_len == 0 || max_bytes == 0) return;
    if (max_bytes > data_len) max_bytes = data_len;
    if (out_size <= (max_bytes * 2)) max_bytes = (out_size - 1) / 2;

    for (size_t i = 0; i < max_bytes; i++) {
        snprintf(out + (i * 2), 3, "%02X", data[i]);
    }
    out[max_bytes * 2] = '\0';
}

static void mid_make_safe_nick(char *out, size_t out_size, const uint8_t *nick, uint16_t nick_len)
{
    if (out == NULL || out_size == 0) return;

    size_t o = 0;
    if (nick != NULL) {
        for (uint16_t i = 0; i < nick_len && o + 1 < out_size; i++) {
            uint8_t c = nick[i];
            if (c >= 0x20 && c <= 0x7E) out[o] = (char)c;
            else out[o] = '.';
            o++;
        }
    }
    out[o] = '\0';
}

void mid_print_peer_table(MidState *s, const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], const char *title)
{
    if (!s || !chat_id) return;

    mid_lock(s);

    time_t now = time(NULL);
    const MidGroupState *g = mid_find_group_const(s, chat_id);

    char chat_id_hex[9];
    mid_hex_short(chat_id_hex, sizeof(chat_id_hex), chat_id, TOX_GROUP_CHAT_ID_SIZE, 4);

    printf("\n");
    printf("==========================================================================================\n");
    printf("MIDDLEWARE PEER TABLE: %s (ChatID %s...)\n", title != NULL ? title : "unknown", chat_id_hex);

    if (g == NULL) {
        printf("(Group not found in middleware)\n");
        printf("==========================================================================================\n\n");
        fflush(stdout);
        mid_unlock(s);
        return;
    }

    size_t active_count = 0;
    size_t online_count = 0;
    size_t signed_count = 0;

    for (size_t i = 0; i < g->count; i++) {
        if (g->records[i].status == MID_STATUS_ACTIVE) active_count++;
        if (g->records[i].connection_status != TOX_CONNECTION_NONE) online_count++;
        if (g->records[i].has_signature) signed_count++;
    }

    printf("records=%zu active=%zu online=%zu signed=%zu sent=%llu recv=%llu\n",
           g->count, active_count, online_count, signed_count,
           (unsigned long long)s->sent_bytes, (unsigned long long)s->recv_bytes);

    printf("+----+------+--------------+--------------+--------------+--------+------+--------+--------+----------+\n");
    printf("| %2s | %-4s | %-12s | %-12s | %-12s | %-6s | %-4s | %-6s | %-6s | %-8s |\n",
           "#", "Self", "Nickname", "Identity", "Signing", "Status", "Sig", "Conn", "Role", "Last seen");
    printf("+----+------+--------------+--------------+--------------+--------+------+--------+--------+----------+\n");

    for (size_t i = 0; i < g->count; i++) {
        const MidPeerRecord *rec = &g->records[i];

        char nick_buf[13];
        char identity_buf[13];
        char signing_buf[13];
        char seen_buf[16];

        mid_make_safe_nick(nick_buf, sizeof(nick_buf), rec->nickname, rec->nickname_len);
        mid_hex_short(identity_buf, sizeof(identity_buf), rec->identity_key, MID_IDENTITY_KEY_SIZE, 6);
        mid_hex_short(signing_buf, sizeof(signing_buf), rec->signing_key, MID_SIGNING_KEY_SIZE, 6);

        bool is_self = false;
        if (g->have_keys) {
            is_self = mid_same_identity(rec->identity_key, g->self_identity_key);
        }

        const char *status_str = "?";
        if (rec->status == MID_STATUS_ACTIVE) status_str = "ACTIVE";
        else if (rec->status == MID_STATUS_LEFT) status_str = "LEFT";

        const char *conn_str = "NONE";
        if (rec->connection_status == TOX_CONNECTION_TCP) conn_str = "TCP";
        else if (rec->connection_status == TOX_CONNECTION_UDP) conn_str = "UDP";

        const char *role_str = "USER";
        if (rec->role == TOX_GROUP_ROLE_FOUNDER) role_str = "FOUNDER";
        else if (rec->role == TOX_GROUP_ROLE_MODERATOR) role_str = "MOD";
        else if (rec->role == TOX_GROUP_ROLE_OBSERVER) role_str = "OBS";

        if (rec->connection_status != TOX_CONNECTION_NONE) {
            snprintf(seen_buf, sizeof(seen_buf), "now");
        } else if (rec->last_seen == 0) {
            snprintf(seen_buf, sizeof(seen_buf), "-");
        } else if (rec->last_seen > (uint64_t)now) {
            snprintf(seen_buf, sizeof(seen_buf), "future");
        } else {
            unsigned long age = (unsigned long)(now - (time_t)rec->last_seen);
            snprintf(seen_buf, sizeof(seen_buf), "%lus", age);
        }

        printf("| %2zu | %-4s | %-12s | %-12s | %-12s | %-6s | %-4s | %-6s | %-6s | %-8s |\n",
               i,
               is_self ? "YES" : "no",
               nick_buf,
               identity_buf,
               signing_buf,
               status_str,
               rec->has_signature ? "yes" : "no",
               conn_str,
               role_str,
               seen_buf);
    }

    printf("+----+------+--------------+--------------+--------------+--------+------+--------+--------+----------+\n");
    printf("\n");
    fflush(stdout);

    mid_unlock(s);
}

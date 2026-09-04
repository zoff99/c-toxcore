# `mid_roster` — Persistent Group Roster Middleware for Tox NGC

**Complete Implementation Specification — Version 3.0**

This document is a complete, from-scratch specification of the `mid_roster` middleware as implemented in `mid_roster.c` / `mid_roster.h`. A C programmer should be able to delete the existing implementation and rewrite it using only this document.

---

## Table of Contents

- [1. Overview](#1-overview)
- [2. Problem Statement](#2-problem-statement)
- [3. Requirements & Dependencies](#3-requirements--dependencies)
- [4. Architecture & Thread-Safety](#4-architecture--thread-safety)
- [5. Key Model (CRITICAL)](#5-key-model-critical)
- [6. Data Structures](#6-data-structures)
- [7. Wire Protocol](#7-wire-protocol)
- [8. Cryptography & Fingerprint Tracking](#8-cryptography--fingerprint-tracking)
- [9. Roster Semantics](#9-roster-semantics)
- [10. Core Algorithm: Record Upsert](#10-core-algorithm-record-upsert)
- [11. Public API Reference](#11-public-api-reference)
- [12. Hook Behavior & Sequence Flows](#12-hook-behavior--sequence-flows)
- [13. Timing Constants & Magic Bytes](#13-timing-constants--magic-bytes)
- [14. Local Persistence (Save/Load)](#14-local-persistence-saveload)
- [15. Integration Guide (Step by Step)](#15-integration-guide-step-by-step)
- [16. Pitfalls & Edge Cases](#16-pitfalls--edge-cases)
- [17. Validation Checklist](#17-validation-checklist)

---

## 1. Overview

`mid_roster` is a thin C middleware layer that sits between a Tox client and Toxcore's NGC (Next Generation Conferences) group API. It maintains a **persistent, cryptographically signed peer roster** that:

- Survives peers going offline (offline peers remain in the list).
- Survives *your own* client restart (via local disk persistence AND batched roster sync from online peers).
- Proves *voluntary departure* using **signed LEFT tombstones**.
- Handles **kicks** by permanently removing the kicked peer.
- Minimizes network traffic using **lightweight heartbeats** and **multicast-suppressed batched roster sync**.
- Requires **no server** — pure peer-to-peer, eventually consistent.
- Supports **optional local encrypted persistence** via `toxencryptsave`.

Transport is exclusively `tox_group_send_custom_packet()` (lossless). All crypto is libsodium Ed25519.

### Design philosophy

| Principle | Consequence |
|---|---|
| Best-effort, eventually consistent | No consensus, no ordering guarantees; last-writer-wins by timestamp |
| Self-signed records only | A peer only ever signs statements about *itself* |
| Stateless transport | Every PRESENCE record is fully self-contained and verifiable |
| Opaque state | Client sees only `MidState *`; all internals are hidden |
| Thread-safe | Non-recursive mutex protects state; callbacks fire outside the lock |
| Event-driven | Client forwards Toxcore callbacks; middleware does the rest |

---

## 2. Problem Statement

Toxcore NGC gives each client only a list of **currently online** peers. When a peer disconnects (timeout, network loss), Toxcore forgets them. When *you* restart your client and rejoin, you start with an empty peer list.

The middleware adds:

| Feature | Without middleware | With middleware |
|---|---|---|
| See offline members | ❌ | ✅ (status = ACTIVE, connection = NONE) |
| Know who *permanently left* | ❌ | ✅ (signed LEFT tombstone) |
| Kicked peers removed | partial | ✅ |
| Full roster after own restart | ❌ | ✅ (Local save + batched roster sync with fingerprint suppression) |
| Cryptographic proof of membership | ❌ | ✅ (Ed25519 signatures) |
| Low bandwidth keep-alive | ❌ | ✅ (Lightweight 41-byte Heartbeat packets) |

---

## 3. Requirements & Dependencies

### 3.1 Libraries

| Dependency | Used for |
|---|---|
| `toxcore` (NGC build) | Group API |
| `toxencryptsave` | Optional local encrypted save/load |
| `libsodium` | `crypto_sign_detached`, `crypto_sign_verify_detached`, `crypto_sign_ed25519_pk_to_curve25519`, `crypto_sign_ed25519_sk_to_pk`, `sodium_memzero`, `sodium_init` |
| `pthread` | Non-recursive mutex for thread-safety |

### 3.2 Required Toxcore API functions

**Standard NGC API:**

| Function | Purpose |
|---|---|
| `tox_group_send_custom_packet()` | Transport for PRESENCE / HEARTBEAT / ROSTER_BATCH |
| `tox_group_self_get_public_key()` | Our identity key |
| `tox_group_self_get_peer_id()` | Detect "we were kicked" |
| `tox_group_peer_get_public_key()` | Peer identity key |
| `tox_group_peer_by_public_key()` | Online-state check / find peer_id by key |
| `tox_group_get_chat_id()` | Map `group_number` to persistent `chat_id` |

**Patched API (must be added to toxcore):**

| Function | Purpose |
|---|---|
| `tox_group_self_get_signing_public_key()` | Our Ed25519 group signing public key |
| `tox_group_self_get_signing_secret_key()` | Our Ed25519 group signing secret key (64 bytes) |
| `tox_group_peer_get_signing_public_key()` | A peer's Ed25519 signing public key |

> ⚠️ Without the three patched functions the middleware **cannot sign or verify anything** and will not work.

---

## 4. Architecture & Thread-Safety

```text
┌─────────────────────────────────────────────────────────────────┐
│                          TOX CLIENT APP                          │
│        (UI / storage / main loop — knows nothing about crypto)   │
└───────────────┬───────────────────────────────────▲─────────────┘
                │ forward Toxcore events            │ query API
                │ (hooks take group_number)         │ (queries take chat_id)
                ▼                                   │ 
┌─────────────────────────────────────────────────────────────────┐
│                       mid_roster MIDDLEWARE                      │
│                                                                  │
│   MidState ──► pthread_mutex_t (non-recursive)                   │
│            ──► MidGroupState[] ──► MidPeerRecord[]               │
│                                                                  │
│   responsibilities:                                              │
│     • fetch/hold our group keys                                  │
│     • sign & broadcast presence / lightweight heartbeats         │
│     • verify & upsert foreign signed records                     │
│     • answer ROSTER_REQUEST with batched ROSTER_BATCH            │
│     • multicast suppression via roster_fingerprint               │
│     • tombstone garbage collection (7-day TTL)                   │
│     • save/load state to disk (optional encryption)              │
└───────────────┬───────────────────────────────────▲─────────────┘
                │ tox_group_send_custom_packet()     │ callbacks
                │ key query functions                │ (fired OUTSIDE lock)
                ▼                                   │ 
┌─────────────────────────────────────────────────────────────────┐
│                        Toxcore (NGC)                             │
│           group chat transport over UDP / TCP relays             │
└─────────────────────────────────────────────────────────────────┘
```

### Thread-Safety Model
All public API functions acquire a single **non-recursive `pthread_mutex_t`** before touching internal state. Internal `_internal` helper functions assume the lock is ALREADY held and must NEVER be called without it. Callbacks are always invoked **AFTER** the lock is released to prevent deadlocks when the client calls back into the middleware from within the callback.

---

## 5. Key Model (CRITICAL)

Tox NGC uses several different keys. Confusing them is the #1 implementation mistake. The middleware uses exactly **three** keys per group member:

| Name | Size | Type | What it is | How to obtain (self) | How to obtain (peer) |
|---|---|---|---|---|---|
| `identity_key` | 32 B | Curve25519 public key | The persistent NGC group identity; the **roster lookup key** | `tox_group_self_get_public_key()` | `tox_group_peer_get_public_key()` |
| `signing_key` | 32 B | Ed25519 public key | Verifies signatures made by this peer | `tox_group_self_get_signing_public_key()` [patched] | `tox_group_peer_get_signing_public_key()` [patched] |
| `self_secret_key` | 64 B | Ed25519 secret key | Signs our own records. **Never transmitted over network.** | `tox_group_self_get_signing_secret_key()` [patched] | — never available for peers — |

### 5.1 Key binding

The middleware must verify that a `signing_key` actually belongs to an `identity_key`. In the NGC extended-key layout:

```text
identity_key == curve25519_from_ed25519(signing_key)
```

**`mid_valid_key_binding(identity, signing)` algorithm:**

```text
1. if identity is all-zero OR signing is all-zero  → REJECT
2. if identity == signing (byte-equal)             → ACCEPT   (compatibility)
3. derived = crypto_sign_ed25519_pk_to_curve25519(signing)
   if derivation fails                             → REJECT
4. if identity == derived                          → ACCEPT
5. else                                            → REJECT
```

---

## 6. Data Structures

### 6.1 Enumerations

```c
typedef enum {
    MID_STATUS_ACTIVE = 0,   // peer is (or was) a member
    MID_STATUS_LEFT   = 1    // peer permanently left (tombstone)
} MidStatus;

typedef enum {
    MID_MSG_PRESENCE       = 1,  // full signed presence record
    MID_MSG_ROSTER_REQUEST = 2,  // request full roster
    MID_MSG_HEARTBEAT      = 3,  // lightweight keep-alive
    MID_MSG_ROSTER_BATCH   = 4   // batched roster response
} MidMsg;
```

### 6.2 `MidPeerRecord` — one roster entry (internal)

```c
typedef struct {
    uint8_t  identity_key[32];      // roster primary key
    uint8_t  signing_key[32];       // Ed25519 verification key
    uint8_t  status;                // MidStatus
    uint64_t timestamp;             // sender-generated unix seconds
    uint8_t  nickname[128];         // raw nickname bytes
    uint16_t nickname_len;          // 0..128
    uint8_t  signature[64];         // Ed25519 detached signature over body
    bool     has_signature;         // false = locally observed only
    Tox_Connection connection_status; // LOCAL observation, never transmitted
    Tox_Group_Role role;            // LOCAL observation, never transmitted
    uint64_t last_seen;             // LOCAL observation, never transmitted
} MidPeerRecord;
```

> **Important:** `connection_status`, `role`, and `last_seen` are *local observations*. They are never part of the signed/transmitted payload.

### 6.3 `MidGroupState` — per-group state (internal)

```c
typedef struct {
    uint8_t  chat_id[32];           // persistent group identifier

    uint8_t  self_identity_key[32];
    uint8_t  self_signing_key[32];
    uint8_t  self_secret_key[64];   // wipe on free/delete
    bool     have_keys;

    uint8_t  self_nickname[128];
    uint16_t self_nickname_len;
    bool     self_active;           // false after mid_announce_leave()
    bool     announced;             // we have announced at least once

    MidPeerRecord *records;
    size_t         count;
    size_t         capacity;

    uint64_t last_announce;         // for heartbeat scheduling
    uint64_t roster_reply_deadline; // multicast suppression timer
    uint8_t  roster_fingerprint[32]; // XOR sum of all signed identity keys
} MidGroupState;
```

### 6.4 `MidState` — top-level opaque state

```c
struct MidState {
    MidGroupState *groups;
    size_t         group_count;
    size_t         group_capacity;

    /* peer-list-changed notification */
    mid_peer_list_changed_cb peer_list_changed_cb;
    void                    *peer_list_changed_user_data;

    pthread_mutex_t *mutex;         // non-recursive thread-safety lock
    uint64_t last_sync_time;        // throttle for heavy sync loop
    uint64_t sent_bytes;            // network stats
    uint64_t recv_bytes;            // network stats
    
    char *save_path;                // local persistence path
};
```

### 6.5 `MidPeerInfo` — public flat peer view (for UI / JNI)

```c
typedef struct {
    uint8_t  identity_key[32];
    uint8_t  signing_key[32];
    uint8_t  status;              // 0 = ACTIVE, 1 = LEFT
    uint8_t  connection_status;   // Tox_Connection: 0=NONE, 1=TCP, 2=UDP
    uint8_t  role;                // Tox_Group_Role
    uint8_t  has_signature;       // 1/0
    uint8_t  reserved;            // padding, always 0
    uint64_t last_seen;           // unix seconds, 0 = never
    uint16_t nickname_len;
    char     nickname[129];       // always NUL-terminated
} MidPeerInfo;
```

---

## 7. Wire Protocol

All messages are sent with `tox_group_send_custom_packet(tox, group, /*lossless=*/true, ...)`. Maximum packet size is bounded to 1200 bytes.

### 7.1 Common header (5 bytes)

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 1 | magic byte 0 | `MID_MAGIC_0` |
| 1 | 1 | magic byte 1 | `MID_MAGIC_1` |
| 2 | 1 | magic byte 2 | `MID_MAGIC_2` |
| 3 | 1 | protocol version | `MID_PROTOCOL_VERSION` |
| 4 | 1 | message type | `1`=PRESENCE, `2`=REQ, `3`=HB, `4`=BATCH |

### 7.2 PRESENCE message (type = 1)

Full signed record containing status, timestamp, nickname, identity key, and signing key. Used for JOINs, LEAVEs, and Name Changes.

### 7.3 HEARTBEAT message (type = 3)

Lightweight keep-alive. Updates `connection_status` and `last_seen` without re-sending nickname or keys.
**Body layout (41 bytes, signed):**
```text
 offset   size   field
 ──────   ────   ──────────────
  0       1      status (ACTIVE)
  1       8      timestamp (uint64 BE)
  9       32     identity_key
```

### 7.4 ROSTER_REQUEST (type = 2) & ROSTER_BATCH (type = 4)

Instead of replying with individual PRESENCE packets, peers reply with a **ROSTER_BATCH** message.

**ROSTER_BATCH layout:**
```text
 offset   size   field
 ──────   ────   ─────────────────────────────────
  0       5      header (magic + version + type=4)
  5       32     roster_fingerprint (XOR sum of signed identity keys)
 37       2      record_count (uint16 BE)
 39       ...    [ signature(64) + body_len(2) + body ] ... repeated
```
*Note: If the roster is too large for one custom packet (>1200 bytes), it is split into multiple ROSTER_BATCH packets, each repeating the 5-byte header and 32-byte fingerprint.*

---

## 8. Cryptography & Fingerprint Tracking

### 8.1 Roster Fingerprint
To enable **multicast suppression**, the middleware maintains a 32-byte `roster_fingerprint` per group. This is an incremental XOR sum of the `identity_key` of every peer that currently holds a valid signature.
- When a peer transitions from unsigned → signed, their key is XORed IN.
- When a peer is deleted or loses signature status, their key is XORed OUT.
- Nickname changes or timestamp updates do NOT affect the fingerprint.

### 8.2 Security rules

| Rule | Rationale |
|---|---|
| A record is only trusted about the identity named inside it | Prevents impersonation |
| If the *sender* claims to be the subject, their Toxcore signing key must match the record | Prevents relayed forgery |
| Only records **self-signed by the subject** are stored long-term | Roster integrity |
| A signed record's `signing_key` is immutable for that identity | Prevents key rotation attacks |

---

## 9. Roster Semantics

### 9.1 The Graveyard (Tombstones)

- A LEFT tombstone is kept in the roster for **`MID_TOMBSTONE_TTL_SEC = 7 days`**.
- Reason: a peer who was offline when someone left must later be able to sync and receive cryptographic proof of the departure.
- `mid_iterate()` purges tombstones with `timestamp < now − TTL`.

### 9.2 Multicast Suppression

When a new peer joins, multiple existing peers might try to send the roster simultaneously. To prevent a broadcast storm:
1. Existing peers schedule a delayed `ROSTER_BATCH` broadcast (1–5 seconds randomized).
2. If an existing peer receives a `ROSTER_BATCH` from another peer *before* its timer fires, and the `roster_fingerprint` matches exactly, it **cancels its own pending response**.

---

## 10. Core Algorithm: Record Upsert

`mid_upsert_record(group, record)` handles the insertion and updating of records.

**Precedence summary:**

| Incoming ↓ / Existing → | unsigned | signed ACTIVE | signed LEFT |
|---|---|---|---|
| **unsigned** | replaces if `ts >=` | never | never |
| **signed ACTIVE** | always | replaces if `ts >` | replaces if `ts >` |
| **signed LEFT** | always | replaces if `ts >=` | replaces if `ts >=` |

> The `>=` for tombstones guarantees a LEFT with the *same* timestamp as the last ACTIVE still wins.

---

## 11. Public API Reference

> **API Note:** Event hooks (`mid_on_group_*`) take `group_number` because Toxcore callbacks provide it. Query and Action functions (`mid_peer_count`, `mid_find_peer`, etc.) take the persistent `chat_id` (32 bytes) because `group_number` can change across client restarts.

### 11.1 Lifecycle

| Function | Description |
|---|---|
| `MidState *mid_new(const char *save_path, const uint8_t *passphrase, size_t passphrase_len)` | `sodium_init()`, allocate state, init mutex. Loads from disk if `save_path` is provided. |
| `bool mid_save(MidState *s, const uint8_t *passphrase, size_t passphrase_len)` | Serializes state and writes atomically to disk (encrypted if passphrase provided). |
| `void mid_free(MidState *s)` | Wipes keys, frees records, destroys mutex. |

### 11.2 Event hooks (call from Toxcore callbacks)

| Function | When to call |
|---|---|
| `mid_on_group_self_join(s, tox, group, nick, len)` | We joined or created a group |
| `mid_on_group_delete(s, tox, group)` | We leave/delete a group |
| `mid_on_group_peer_join(s, tox, group, peer_id)` | `group_peer_join` callback |
| `mid_on_group_peer_exit(s, tox, group, exit_type)` | `group_peer_exit` callback |
| `mid_on_group_peer_name(s, tox, group, peer_id)` | `group_peer_name` callback |
| `mid_on_group_moderation(s, tox, group, src, dst, type)` → `bool` | Returns **true if WE were kicked** |
| `mid_on_group_custom_packet(s, tox, group, peer_id, data, len)` | `group_custom_packet` callback |
| `mid_iterate(s, tox)` | Every main-loop iteration |

### 11.3 Actions

| Function | Description |
|---|---|
| `bool mid_announce_leave(s, tox, group)` | Broadcast signed LEFT tombstone. Call **immediately before** `tox_group_leave()`. |
| `bool mid_self_set_name(s, tox, group, nick, len)` | Update our own nickname, update local record, and re-announce signed PRESENCE. |
| `bool mid_delete_peer_by_identity(s, chat_id, id_key)` | Manually remove a peer. **Required for the kicker**. |

### 11.4 Queries (Take `chat_id`)

| Function | Returns |
|---|---|
| `mid_group_count(s)` | Number of managed groups |
| `mid_peer_count(s, chat_id)` | Total roster entries |
| `mid_signed_count(s, chat_id)` | Entries with valid signatures |
| `mid_online_count(s, chat_id)` | Entries currently observed online |
| `mid_find_peer(s, chat_id, id_key)` | Index ≥ 0, or −1 |
| `mid_peer_is_signed_left(s, chat_id, id_key)` | true if a signed LEFT tombstone exists |

### 11.5 Flat peer-list API (for UI / JNI)

```c
size_t mid_peer_list_count(MidState *s, const uint8_t chat_id[32]);
bool   mid_peer_list_get(MidState *s, const uint8_t chat_id[32], size_t index, MidPeerInfo *out);
```

### 11.6 Change notification

```c
typedef void (*mid_peer_list_changed_cb)(const uint8_t chat_id[32], void *user_data);
void mid_set_peer_list_changed_cb(MidState *s, mid_peer_list_changed_cb cb, void *user_data);
```
Called **outside the mutex lock**. It is safe to call `mid_peer_list_get()` from within the callback.

### 11.7 Utilities

```c
void mid_get_network_stats(MidState *s, uint64_t *sent_bytes, uint64_t *recv_bytes);
void mid_print_peer_table(MidState *s, const uint8_t chat_id[32], const char *title);
```

---

## 12. Hook Behavior & Sequence Flows

### 12.1 `mid_on_group_peer_join` (Reliability Fix)

Toxcore often drops custom packets sent immediately during the join handshake. Instead of unicasting immediately, existing peers schedule a delayed `ROSTER_BATCH` broadcast (1–5s). The fingerprint suppression ensures only ONE existing peer actually sends it.

### 12.2 `mid_on_group_custom_packet`

- **HEARTBEAT (Type 3):** Updates `connection_status`, `role`, and `last_seen`. Does NOT overwrite full signed records or modify `timestamp`.
- **ROSTER_BATCH (Type 4):** Parses the batch. If structurally complete and `sender_fp == our_fp`, cancels our pending `roster_reply_deadline` (multicast suppression).

### 12.3 `mid_iterate` (Throttled)

1. **Throttle:** Heavy sync only runs every `MID_SYNC_INTERVAL_SEC` (5 seconds) to avoid hammering Toxcore on 50ms ticks.
2. **Sync:** Marks vanished peers offline via `tox_group_peer_by_public_key()`.
3. **Heartbeat:** If `now - last_announce >= 300s`, sends lightweight `HEARTBEAT` packet.
4. **Tombstone GC:** Purges LEFT records older than 7 days.
5. **Roster Timer:** If `roster_reply_deadline` is reached, sends `ROSTER_BATCH`.

---

## 13. Timing Constants & Magic Bytes

| Constant | Value | Purpose |
|---|---|---|
| `MID_MAGIC_0` / `1` / `2` | `0xA0` / `0x91` / `...` | Packet identification |
| `MID_PROTOCOL_VERSION` | `1` | Version gate |
| `MID_HEARTBEAT_SEC` | `300` (5 min) | Lightweight keep-alive interval |
| `MID_SYNC_INTERVAL_SEC` | `5` | Throttle for heavy sync loop |
| `MID_ROSTER_COOLDOWN_SEC` | `30` | Min gap between roster responses |
| `MID_TOMBSTONE_TTL_SEC` | `604800` (7 d) | Graveyard retention |
| `MID_MAX_PACKET_SIZE` | `1200` | Maximum custom packet payload size |

---

## 14. Local Persistence (Save/Load)

If a `save_path` is provided to `mid_new()`, the middleware attempts to load the roster from disk. Calling `mid_save()` writes the current state back atomically (using a `.tmp` file and `rename()`).

### 14.1 Encryption
If a `passphrase` is provided, the entire serialized payload is encrypted using `tox_pass_encrypt()` (from `toxencryptsave`). The resulting file begins with the standard Tox encryption magic bytes. Upon loading, `tox_is_data_encrypted()` detects this and decrypts it before parsing.

### 14.2 Binary File Format (`MIDR`)
The unencrypted payload follows this exact binary layout (all multi-byte integers are Big-Endian):

```text
[Header]
- Magic:        "MIDR" (4 bytes)
- Version:      uint32 (Must be 2)
- Group Count:  uint32

[Group Data] (Repeated Group Count times)
- chat_id:             32 bytes
- self_identity_key:   32 bytes
- self_signing_key:    32 bytes
- self_secret_key:     64 bytes
- self_nickname_len:   uint16
- self_nickname:       N bytes
- peer_count:          uint32

  [Peer Data] (Repeated peer_count times)
  - identity_key:      32 bytes
  - signing_key:       32 bytes
  - status:            uint8
  - timestamp:         uint64
  - nickname_len:      uint16
  - nickname:          N bytes
  - signature:         64 bytes
  - has_signature:     uint8 (1 or 0)
  - connection_status: uint32
  - role:              uint32
  - last_seen:         uint64
```

> **Note on Loading:** When peers are loaded from disk, their `connection_status` is intentionally forced to `TOX_CONNECTION_NONE` (offline). This forces the middleware to do a fresh online-state sync with Toxcore on the next `mid_iterate()` tick.

---

## 15. Integration Guide (Step by Step)

### Step 1 — Add state to your client object

```c
#include "mid_roster.h"

typedef struct {
    Tox      *tox;
    MidState *mid;            
    uint32_t  group_number;   
    uint8_t   chat_id[32];    // ← Cache the chat_id for queries!
} MyClient;
```

### Step 2 — Initialize with Persistence

```c
// On startup
c->mid = mid_new("/path/to/roster.mid", (const uint8_t *)"my_password", 11);

// On shutdown
mid_save(c->mid, (const uint8_t *)"my_password", 11);
mid_free(c->mid);
```

### Step 3 — Forward every group callback

```c
static void group_self_join_cb(Tox *tox, uint32_t group_number, void *ud)
{
    MyClient *c = ud;
    c->group_number = group_number;
    
    // Cache chat_id for future queries
    tox_group_get_chat_id(tox, group_number, c->chat_id, NULL);
    
    mid_on_group_self_join(c->mid, tox, group_number,
                           (const uint8_t *)c->name, strlen(c->name));
}

static void group_custom_packet_cb(Tox *tox, uint32_t g, uint32_t peer_id,
                                   const uint8_t *data, size_t len, void *ud)
{
    MyClient *c = ud;
    mid_on_group_custom_packet(c->mid, tox, g, peer_id, data, len);
}
```

### Step 4 — Render the roster in your UI (Using `chat_id`)

```c
static void on_peer_list_changed(const uint8_t chat_id[32], void *ud)
{
    MyClient *c = ud;
    // Note: mid_peer_list_count takes chat_id, not group_number!
    size_t n = mid_peer_list_count(c->mid, chat_id);
    for (size_t i = 0; i < n; i++) {
        MidPeerInfo p;
        if (!mid_peer_list_get(c->mid, chat_id, i, &p)) continue;
        /* push p into your UI model */
    }
}
```

---

## 16. Pitfalls & Edge Cases

| # | Pitfall | Correct behavior |
|---|---|---|
| 16.1 | Founder never announces | Call `mid_on_group_self_join` manually after `tox_group_new` |
| 16.2 | Kicked peer stays in kicker's roster | Kicker must call `mid_delete_peer_by_identity` |
| 16.3 | Deadlock in callback | Callbacks fire OUTSIDE the mutex lock; safe to call queries |
| 16.4 | Internal function calls public API | `_internal` functions must NEVER call public wrappers (deadlock) |
| 16.5 | Queries fail after restart | Use `chat_id` for queries, not `group_number` |
| 16.6 | Broadcast storm on peer join | Solved by randomized delay + fingerprint suppression |
| 16.7 | High CPU usage in main loop | `mid_iterate` throttles heavy sync to every 5 seconds |
| 16.8 | Saving blocks the UI thread | `mid_save` releases the mutex *before* doing blocking Disk I/O |

---

## 17. Validation Checklist

A correct implementation must pass this scenario sequence:

| Phase | Scenario | Expected result |
|---|---|---|
| 1 | 3 clients join one private group | All 3 rosters show 3 **signed** peers |
| 2 | Client 3 hard-disconnects | Others detect exit; client 3 stays in roster as offline ACTIVE |
| 3 | Client 4 joins late | Receives `ROSTER_BATCH`, syncs full persistent roster |
| 4 | Client 2 gracefully leaves | `mid_announce_leave` → `tox_group_leave`; others hold signed LEFT tombstone |
| 5 | Heartbeat test | Wait 5 minutes; observe lightweight `HEARTBEAT` packets (41 bytes) instead of full PRESENCE |
| 6 | Name change test | Call `mid_self_set_name`; observe new signed PRESENCE broadcast and UI update |
| 7 | Restart test | Kill Client 1, restart it. It loads from disk and immediately syncs missing offline states via Toxcore. |
| ∞ | Tombstone GC | LEFT records older than 7 days purged by `mid_iterate` |

---

*End of specification.*
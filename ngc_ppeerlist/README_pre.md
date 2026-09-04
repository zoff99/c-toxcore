# Tox NGC Persistent Roster Middleware — `mid_roster` Specification

**Version:** 1.0
**Target reader:** A C programmer who wants to (re)implement or integrate this middleware.
**Dependencies:** `toxcore` (with the NGC signing-key API), `libsodium`.

---

## 1. Overview

### 1.1 Purpose

Tox NGC (New Group Chats) only keeps a peer in the peer list **while that peer is online and connected**. When a peer goes offline, Toxcore removes it from the live peer list, so other peers "forget" that the peer was ever in the group.

`mid_roster` adds a **persistent, eventually-consistent group roster** on top of Tox NGC using only `tox_group_send_custom_packet()`. It lets every client remember **all** members of a group — including members who are currently offline — and lets a brand-new joiner learn the full member history.

### 1.2 Design goals

| Goal | How it is achieved |
|---|---|
| No central server | Pure peer-to-peer gossip over group custom packets |
| Authenticity | Every membership change is signed with the member's Ed25519 group key |
| Offline peers remembered | Signed records persist in each client's roster, independent of connectivity |
| Offline peers can re-sync | Signed records are forwarded to late joiners on request |
| Graceful leave is permanent | A signed `LEFT` tombstone propagates and overrides earlier `ACTIVE` records |
| Easy integration | A small, opaque C API driven entirely from Tox callbacks + one loop call |

### 1.3 Non-goals (deliberate limitations)

- **Not strongly consistent.** It is *eventually* consistent. During a network partition, some peers will temporarily disagree.
- **No trusted "last seen" for offline peers.** `last_seen` is a **local, transient** observation, not an authenticated global fact.
- **No trusted global timestamp.** Timestamps are sender-generated and used only for per-identity ordering, not wall-clock truth.
- **No persistence across client restart is built in.** The middleware keeps state in RAM. Persistence is delegated to the application through save/load hooks.

---

## 2. Key Concepts

| Term | Meaning |
|---|---|
| **Identity key** | The normal 32-byte Tox NGC group peer public key (`tox_group_peer_get_public_key`). This is the *primary roster lookup key*. |
| **Signing key** | The 32-byte Ed25519 public key that verifies a peer's signatures (`tox_group_peer_get_signing_public_key`). |
| **Secret signing key** | The 64-byte Ed25519 private key a peer uses to sign its own records (`tox_group_self_get_signing_secret_key`). |
| **Record** | A signed statement about one peer: *status + timestamp + nickname + keys*. |
| **Tombstone** | A signed record with `status = LEFT`. It permanently overrides earlier `ACTIVE` records for that identity. |
| **Roster** | The local set of all known records (active + left). |
| **Presence** | Transient online/offline state. Not signed, not persistent. |

> **Core distinction:** *Membership* is persistent and signed. *Presence* is transient and local. Do not conflate them.

---

## 3. High-Level Architecture

```
                        ┌──────────────────────────────────────┐
                        │             Tox Client               │
                        │  (your app: UI, storage, logic)      │
                        └───────────────┬──────────────────────┘
                                        │  forwards Tox events
                                        ▼
                        ┌──────────────────────────────────────┐
                        │           mid_roster (this)          │
                        │  - roster table (records[])          │
                        │  - signing / verification            │
                        │  - gossip (custom packets)           │
                        │  - heartbeat + roster sync           │
                        └───────────────┬──────────────────────┘
                                        │  tox_group_send_custom_packet()
                                        ▼
                        ┌──────────────────────────────────────┐
                        │             toxcore (NGC)            │
                        │  (group chat transport over DHT/TCP) │
                        └──────────────────────────────────────┘
```

The middleware is a **passenger** on the Tox group transport. It never opens sockets. It:
1. Receives events *from* the client (who joined, who left, incoming custom packet).
2. Maintains the roster table.
3. Emits signed records *back out* through `tox_group_send_custom_packet()`.

---

## 4. Cryptographic Identity Model

### 4.1 The three keys

Tox NGC uses an **extended keypair**. The middleware needs three distinct keys:

| Key | Size | Obtained via | Purpose |
|---|---|---|---|
| Identity key | 32 bytes | `tox_group_self_get_public_key` / `tox_group_peer_get_public_key` | Roster lookup key; stable identity |
| Signing public key | 32 bytes | `tox_group_self_get_signing_public_key` / `tox_group_peer_get_signing_public_key` | Verify signatures |
| Signing secret key | 64 bytes | `tox_group_self_get_signing_secret_key` | Sign our own records |

> ⚠️ The **identity key is NOT the signing key.** The identity key is the Curve25519 encryption key; the signing key is a separate Ed25519 key. You must fetch both.

### 4.2 Key binding (anti-spoofing)

A malicious peer could try to attach a signing key to someone else's identity key. To prevent this, every record must satisfy a **binding check** before it is accepted:

```
valid_binding(identity, signing) =
        (identity == signing)                                   # 32-byte equality
     OR (curve25519_from_ed25519(signing) == identity)        # derivation
```

- The first branch handles implementations where the exposed identity key *is* the signing key.
- The second branch handles the normal NGC extended-key case, where the identity key is the Curve25519 key *derived from* the Ed25519 signing key.

If neither holds, the record is rejected as forged.

### 4.3 What is signed

A record's **body** (see §6.3) is signed with the member's own secret signing key. Because the identity key and signing key are both *inside* the signed body, a signature proves:

> "The owner of this signing key asserts this status/nickname for this identity."

Combined with the binding check (§4.2), this proves the record came from the owner of the identity.

---

## 5. Data Structures

### 5.1 `MidPeerRecord` — one roster entry

```c
typedef struct {
    uint8_t  identity_key[MID_IDENTITY_KEY_SIZE];  // 32 — primary key
    uint8_t  signing_key[MID_SIGNING_KEY_SIZE];    // 32 — verifies signatures

    uint8_t  status;        // MID_STATUS_ACTIVE (0) or MID_STATUS_LEFT (1)
    uint64_t timestamp;     // sender-generated, for per-identity ordering

    uint8_t  nickname[MID_MAX_NICK_SIZE];          // 128
    uint16_t nickname_len;

    uint8_t  signature[MID_SIG_SIZE];              // 64 — Ed25519 over body
    bool     has_signature;

    bool     online;        // TRANSIENT — local observation only
    uint64_t last_seen;     // TRANSIENT — local observation only
} MidPeerRecord;
```

| Field | Persistent? | Signed? | Notes |
|---|---|---|---|
| `identity_key` | ✅ | ✅ | Primary key |
| `signing_key` | ✅ | ✅ | For verification |
| `status` | ✅ | ✅ | ACTIVE or LEFT |
| `timestamp` | ✅ | ✅ | Ordering |
| `nickname` | ✅ | ✅ | Display name |
| `signature` | ✅ | — | The signature itself |
| `online` | ❌ | ❌ | Local only |
| `last_seen` | ❌ | ❌ | Local only |

### 5.2 `MidState` — one middleware instance

```c
typedef struct {
    uint8_t  self_identity_key[MID_IDENTITY_KEY_SIZE];
    uint8_t  self_signing_key[MID_SIGNING_KEY_SIZE];
    uint8_t  self_secret_key[MID_SIGNING_SECRET_KEY_SIZE]; // wipe on free!
    bool     have_keys;

    uint8_t  self_nickname[MID_MAX_NICK_SIZE];
    uint16_t self_nickname_len;
    bool     self_active;

    MidPeerRecord *records;   // dynamic array = the roster
    size_t         count;
    size_t         capacity;

    uint64_t last_announce;          // last time we broadcast our own record
    uint64_t last_roster_response;   // rate-limit for roster responses
} MidState;
```

> One `MidState` can manage **multiple groups** if you keep one roster per group. In the simple one-group test, there is one `MidState`.

---

## 6. Wire Protocol

### 6.1 Framing

Every middleware packet begins with a 4-byte header:

```
offset  size  field
─────   ────  ─────────────────────────
0       1     MID_MAGIC_0        = 0xC7
1       1     MID_MAGIC_1        = 0x91
2       1     MID_PROTOCOL_VERSION = 1
3       1     message_type       (see below)
4       …     payload
```

The magic bytes let the middleware ignore custom packets that belong to other protocols sharing the same group.

### 6.2 Message types

| Value | Name | Direction | Payload |
|---|---|---|---|
| `1` | `MID_MSG_PRESENCE` | broadcast | One signed record |
| `2` | `MID_MSG_ROSTER_REQUEST` | broadcast | empty (or optional) |

### 6.3 `PRESENCE` payload layout

```
offset  size              field
─────   ────────────────  ──────────────────────────────────────
0       64                signature (Ed25519 over the body below)
64      …                 ── record body (the part that is signed) ──
  +0      1               status            (0=ACTIVE, 1=LEFT)
  +1      8               timestamp         (big-endian)
  +9      2               nickname_len      (big-endian)
  +11     nickname_len    nickname bytes
  …       2               identity_key_len  (big-endian)
  …       identity_key_len identity_key
  …       32              signing_key
```

> All multi-byte integers are **big-endian**.
> The **signature covers the body only** (everything after the 64-byte signature), not the signature itself and not the 4-byte packet header.

### 6.4 `ROSTER_REQUEST` payload

Empty. It simply asks "does anyone have records I'm missing?" Receivers respond by broadcasting their signed records (see §8.4).

---

## 7. Record Lifecycle

```
                    ┌─────────────────────────────┐
                    │        peer joins group      │
                    └───────────────┬─────────────┘
                                    │ signs ACTIVE record, broadcasts
                                    ▼
                    ┌─────────────────────────────┐
                    │   ACTIVE record in roster    │◄────────────┐
                    │   online = true              │             │ heartbeat
                    └───────────────┬─────────────┘             │ re-signs &
                                    │                            │ re-broadcasts
                ┌───────────────────┼───────────────────┐        │
                │                   │                   │        │
       peer goes offline    peer sends LEFT      peer kicked/timeout
                │                   │                   │
                ▼                   ▼                   ▼
        online=false         LEFT tombstone       online=false
        (still in roster)    (status=LEFT,        (local only)
                             overrides ACTIVE)
```

Key points:
- Going **offline** only sets `online = false`. The record stays in the roster.
- **Leaving** produces a signed `LEFT` tombstone that is *gossiped* and *overrides* earlier `ACTIVE` records.
- Being kicked or timing out is detected locally (via Tox exit callback) and only flips `online`.

---

## 8. Core Algorithms

### 8.1 Upsert (conflict resolution)

This is the heart of the middleware. When a record arrives (over the wire or locally), it is merged into the roster:

```
upsert(incoming):
    normalize(incoming)                       # clamp nickname, coerce status
    if incoming.has_signature and not verify(incoming): REJECT

    existing = find_by_identity(incoming.identity_key)

    if not found:
        insert(incoming); return

    # Signing keys must never change for the same identity
    if existing.has_signature and incoming.has_signature
       and existing.signing_key != incoming.signing_key: REJECT

    replace = false
    if incoming.has_signature:
        # signed beats unsigned; newer signed beats older signed
        replace = (not existing.has_signature) or (incoming.timestamp > existing.timestamp)
    else:
        # unsigned only replaces unsigned, and only if not older
        replace = (not existing.has_signature) and (incoming.timestamp >= existing.timestamp)

    if replace:
        merge(incoming into existing)
        # preserve online flag heuristically
        if incoming.status==ACTIVE and not incoming.online and existing.online:
            keep online = true
        if incoming.status==LEFT: online = false

    # Live observation always refreshes presence (never membership)
    if incoming.online and incoming.status==ACTIVE and incoming.last_seen >= existing.last_seen:
        existing.online = true
        existing.last_seen = incoming.last_seen
```

**Conflict-resolution summary:**

| Incoming | Existing | Rule |
|---|---|---|
| signed, newer timestamp | signed | **replace** |
| signed, older timestamp | signed | keep existing |
| signed | unsigned | **replace** (signed beats unsigned) |
| unsigned, newer | unsigned | **replace** |
| unsigned, older | unsigned | keep existing |
| any | signed with *different* signing key | **REJECT** (spoof) |

### 8.2 Verification

```
verify(record):
    if not record.has_signature: return false
    if not valid_binding(record.identity_key, record.signing_key): return false
    body = pack_body(record)                 # status+timestamp+nick+keys
    return ed25519_verify(record.signature, body, record.signing_key)
```

### 8.3 Heartbeat (self announce)

Inside `mid_iterate()`, if the peer is active and `MID_HEARTBEAT_SEC` has elapsed since the last announce, the peer re-signs and re-broadcasts its own `ACTIVE` record. This:
- keeps the record fresh,
- re-asserts membership after reconnects,
- lets others refresh their view.

### 8.4 Roster request / response

- A peer broadcasts `MID_MSG_ROSTER_REQUEST` when it joins (or suspects it is behind).
- Receivers respond by broadcasting all their signed records.
- Responses are **rate-limited** (`MID_ROSTER_COOLDOWN_SEC`) to avoid flooding when many peers request at once.

### 8.5 Online/offline sync with Tox

`mid_iterate()` also reconciles the transient `online` flags with Tox's live peer list: any record marked `online` whose identity is **not** in Tox's current peer list is flipped to `online = false`. This keeps presence honest without touching the signed membership.

---

## 9. Public API Reference

| Function | Purpose |
|---|---|
| `mid_new()` | Create a middleware instance (initialises libsodium). |
| `mid_free(s)` | Free the instance; **wipes the secret key**. |
| `mid_on_group_self_join(s, tox, group, nick, len)` | Call when *we* join/create a group. Fetches keys, announces self, requests roster. |
| `mid_on_group_delete(s, group)` | Call when we leave/delete a group. Wipes keys & roster. |
| `mid_on_group_peer_join(s, tox, group, peer_id)` | Call when another peer joins. Records them online & re-syncs. |
| `mid_on_group_peer_exit(s, tox, group, exit_type)` | Call when a peer exits. Handles `QUIT` tombstone vs. offline. |
| `mid_on_group_peer_name(s, tox, group, peer_id)` | Call when a peer renames. Updates local nickname. |
| `mid_on_custom_packet(s, tox, group, peer_id, data, len)` | Feed incoming custom packets. Verifies & upserts. |
| `mid_iterate(s, tox, group, now)` | Call every loop tick: heartbeat + online/offline sync. |
| `mid_announce_leave(s, tox, group)` | Broadcast a signed `LEFT` tombstone (call before `tox_group_leave`). |
| `mid_peer_count(s, group)` | Number of roster records. |
| `mid_signed_count(s, group)` | Number of *signed* records. |
| `mid_online_count(s, group)` | Number currently online. |
| `mid_peer_is_signed_left(s, group, identity)` | Is there a signed `LEFT` tombstone for this identity? |
| `mid_print_peer_table(s, group, title)` | Debug: print the roster table. |

---

## 10. Integration Guide (for a novice)

This section shows exactly how to wire the middleware into a Tox NGC client.

### Step 0 — Prerequisites

Make sure your `toxcore` build exposes the three signing-key functions:

- `tox_group_self_get_signing_secret_key()`
- `tox_group_self_get_signing_public_key()`
- `tox_group_peer_get_signing_public_key()`

…and that you link `libsodium`.

### Step 1 — Create the middleware

After you create your `Tox` object:

```c
MidState *mid = mid_new();
```

### Step 2 — Register the Tox callbacks

Register the normal NGC callbacks **and** forward each one into the middleware. Here is the wiring table:

| Tox callback | What to call in the middleware |
|---|---|
| `tox_callback_group_self_join` | `mid_on_group_self_join(mid, tox, group, nick, nick_len)` |
| `tox_callback_group_peer_join` | `mid_on_group_peer_join(mid, tox, group, peer_id)` |
| `tox_callback_group_peer_exit` | `mid_on_group_peer_exit(mid, tox, group, exit_type)` |
| `tox_callback_group_peer_name` | `mid_on_group_peer_name(mid, tox, group, peer_id)` |
| `tox_callback_group_custom_packet` | `mid_on_custom_packet(mid, tox, group, peer_id, data, len)` |

Example callback bodies:

```c
void on_group_self_join(Tox *tox, uint32_t group, void *ud) {
    Client *c = ud;
    mid_on_group_self_join(c->mid, tox, group, (const uint8_t*)c->nick, strlen(c->nick));
}

void on_group_peer_join(Tox *tox, uint32_t group, uint32_t peer_id, void *ud) {
    Client *c = ud;
    mid_on_group_peer_join(c->mid, tox, group, peer_id);
}

void on_group_peer_exit(Tox *tox, uint32_t group, uint32_t peer_id,
                        Tox_Group_Exit_Type exit_type,
                        const uint8_t *name, size_t name_len,
                        const uint8_t *part, size_t part_len, void *ud) {
    Client *c = ud;
    mid_on_group_peer_exit(c->mid, tox, group, exit_type);
}

void on_group_custom_packet(Tox *tox, uint32_t group, uint32_t peer_id,
                            const uint8_t *data, size_t len, void *ud) {
    Client *c = ud;
    mid_on_custom_packet(c->mid, tox, group, peer_id, data, len);
}
```

### Step 3 — Drive the loop

In your main loop, after `tox_iterate()`, call `mid_iterate()`:

```c
while (running) {
    tox_iterate(tox, &client);
    mid_iterate(client.mid, tox, group_number, time(NULL));
    usleep(tox_iteration_interval(tox) * 1000);
}
```

### Step 4 — Leaving a group

**Before** calling `tox_group_leave()`, broadcast the `LEFT` tombstone so others remember you left:

```c
mid_announce_leave(client.mid, tox, group_number);
tox_group_leave(tox, group_number, NULL, 0);
mid_on_group_delete(client.mid, group_number);
```

### Step 5 — (Optional) Persistence

The middleware keeps state in RAM. To persist across restarts, save/load the `records[]` array yourself. A simple approach:

- **Save:** iterate `mid_peer_count()` / a getter and write each record to a file/DB.
- **Load:** read them back and feed them into the middleware at startup (before joining).

Because records are **signed**, you can safely reload them and the middleware will re-verify them.

---

## 11. Worked Scenarios

### Scenario A — Peer joins, then goes offline, then a new peer joins

```
A, B online.  E joins.  → A, B, E all have ACTIVE(E).
E goes offline.          → A, B set online(E)=false, keep the record.
C joins (was offline).   → C requests roster.
A/B broadcast records    → C receives ACTIVE(E) and learns E exists,
                           even though E is offline.
```

✅ `C` sees `E` in the roster (marked offline), even though `E` is offline.

### Scenario B — Peer leaves while others are offline

```
A, B online.  E sends LEFT tombstone.  → A, B store LEFT(E).
E, A, B go offline.
C, D come online (they never saw E).
A or B comes back, syncs → C, D receive LEFT(E) tombstone.
```

✅ `C` and `D` learn that `E` existed and left, via the signed tombstone.

### Scenario C — Why a signed tombstone is needed

Without a tombstone, if `E` joined while `C`/`D` were offline and then everyone who saw `E` joined went offline, `C`/`D` would have no way to know `E` existed. The signed tombstone guarantees the *leave* event itself is propagated.

---

## 12. Edge Cases & Failure Modes

| Case | Behaviour |
|---|---|
| Duplicate record arrives | Upsert is idempotent; same timestamp → no change. |
| Forged record (wrong signing key) | Rejected by binding check or signature check. |
| Signing key changes for an identity | Rejected (identity is bound to one signing key). |
| Older signed record arrives after newer | Rejected (timestamp ordering). |
| Peer crashes (no tombstone) | Detected locally via Tox exit; `online=false` only. No tombstone propagated. |
| Peer leaves gracefully | Signed `LEFT` tombstone propagated; permanent. |
| Network partition | Temporary disagreement; resolves when partitions heal and records re-gossip. |
| Many peers request roster at once | Responses rate-limited by `MID_ROSTER_COOLDOWN_SEC`. |
| Client restart | In-RAM state lost unless the app persists it (see §10 Step 5). |

---

## 13. Constants Reference

| Constant | Value | Meaning |
|---|---|---|
| `MID_IDENTITY_KEY_SIZE` | 32 | Identity key size |
| `MID_SIGNING_KEY_SIZE` | 32 | Signing public key size |
| `MID_SIGNING_SECRET_KEY_SIZE` | 64 | Signing secret key size |
| `MID_MAX_NICK_SIZE` | 128 | Max nickname bytes |
| `MID_SIG_SIZE` | 64 | Ed25519 signature size |
| `MID_MAX_PACKET_SIZE` | 1200 | Max custom packet size used |
| `MID_MAGIC_0` | `0xC7` | Magic byte 0 |
| `MID_MAGIC_1` | `0x91` | Magic byte 1 |
| `MID_PROTOCOL_VERSION` | 1 | Protocol version |
| `MID_MSG_PRESENCE` | 1 | Signed-record message |
| `MID_MSG_ROSTER_REQUEST` | 2 | Roster request message |
| `MID_STATUS_ACTIVE` | 0 | Member is active |
| `MID_STATUS_LEFT` | 1 | Member left (tombstone) |
| `MID_HEARTBEAT_SEC` | 10 | Self re-announce interval |
| `MID_ROSTER_COOLDOWN_SEC` | 30 | Roster-response rate limit |

---

## 14. Summary for Reimplementation

To rebuild this middleware from scratch, a C programmer needs to implement:

1. **Identity handling** — fetch identity + signing + secret keys; enforce the binding check.
2. **Record format** — the exact byte layout in §6, big-endian, signature-over-body.
3. **Ed25519 sign/verify** — via libsodium, over the packed body.
4. **Upsert with conflict rules** — §8.1 (signed beats unsigned; newer timestamp wins; signing key immutable).
5. **Gossip** — broadcast signed records; respond to roster requests (rate-limited).
6. **Heartbeat** — periodically re-sign & re-broadcast our own `ACTIVE` record.
7. **Online/offline sync** — reconcile transient presence with Tox's live peer list.
8. **Tombstones** — on graceful leave, sign & broadcast `LEFT`; let it override `ACTIVE`.
9. **A tiny public API** — the table in §9, driven entirely from Tox callbacks + one loop call.

That is the entire middleware. It is deliberately small, stateless across the network, and eventually consistent — relying on **signed records + gossip** rather than any server.

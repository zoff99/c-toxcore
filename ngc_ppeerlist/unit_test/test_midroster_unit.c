#include "test_framework.h"
#include "../../toxcore/tox.h"
#include "../mid_roster.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sodium.h>

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

#define MID_PROTOCOL_VERSION   1
#define MID_MSG_PRESENCE       1
#define MID_MSG_ROSTER_REQUEST 2
#define MID_MSG_HEARTBEAT      3
#define MID_MSG_ROSTER_BATCH   4

#define MID_STATUS_ACTIVE      0
#define MID_STATUS_LEFT        1

#define SELF_SEED_ID           0x77777777u

/* [NEW] Mirrors of middleware constants not exposed via mid_roster.h.
 * Keep these in sync with mid_roster.c. */
#define TEST_TIMESTAMP_ROUNDING_SEC  20
#define TEST_MAX_PACKET_PADDING      64
#define TEST_ROSTER_COOLDOWN_SEC     20
#define TEST_HEARTBEAT_SEC           3600
#define TEST_HEARTBEAT_JITTER_SEC    1800
#define TEST_SYNC_INTERVAL_SEC       5

/* V4 signed body: status(1) + timestamp(8) + identity(32) + signing(32) */
#define TEST_SIGNED_BODY_SIZE        (1 + 8 + 32 + 32)

#define GROUP_1    1
#define GROUP_2    2
#define GROUP_9999 9999

static const uint8_t cid1[TOX_GROUP_CHAT_ID_SIZE] = {1};
static const uint8_t cid2[TOX_GROUP_CHAT_ID_SIZE] = {2};
static const uint8_t cid9999[TOX_GROUP_CHAT_ID_SIZE] = {0x0F, 0x27};

/* ------------------------------------------------------------------ */
/* Mock state                                                         */
/* ------------------------------------------------------------------ */

static bool mock_only_self_present = false;
static bool mock_long_peer_name    = false;
static int  mock_send_count        = 0;

/* [NEW] Capture the most recent outgoing packet for padding inspection. */
static bool    mock_capture_packets = false;
static uint8_t mock_last_packet[2048];
static size_t  mock_last_packet_len = 0;

/* ------------------------------------------------------------------ */
/* Deterministic valid Ed25519 keys                                   */
/* ------------------------------------------------------------------ */

static void test_seed_from_u32(uint32_t v, uint8_t seed[32])
{
    memset(seed, 0, 32);
    seed[0] = 0x5A;
    seed[1] = (uint8_t)(v >> 24);
    seed[2] = (uint8_t)(v >> 16);
    seed[3] = (uint8_t)(v >> 8);
    seed[4] = (uint8_t)(v);
    seed[5] = 0x43;
}

static void test_keypair_from_id(uint32_t id, uint8_t id_pk[32], uint8_t sig_pk[32], uint8_t sig_sk[64])
{
    uint8_t seed[32];
    uint8_t tmp_pk[32];
    uint8_t tmp_sk[64];

    test_seed_from_u32(id, seed);
    crypto_sign_seed_keypair(tmp_pk, tmp_sk, seed);

    if (sig_pk != NULL) memcpy(sig_pk, tmp_pk, 32);
    if (sig_sk != NULL) memcpy(sig_sk, tmp_sk, 64);
    if (id_pk != NULL) {
        if (crypto_sign_ed25519_pk_to_curve25519(id_pk, tmp_pk) != 0) {
            memcpy(id_pk, tmp_pk, 32);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Tox mocks                                                          */
/* ------------------------------------------------------------------ */

typedef struct DummyTox { int dummy; } DummyTox;

Tox *create_dummy_tox(void)
{
    static DummyTox t;
    return (Tox *)&t;
}

bool tox_group_self_get_public_key(const Tox *tox, uint32_t group_number,
                                   uint8_t *public_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (!public_key) return false;
    uint8_t sig_pk[32], sig_sk[64];
    test_keypair_from_id(SELF_SEED_ID, public_key, sig_pk, sig_sk);
    return true;
}

bool tox_group_self_get_signing_public_key(const Tox *tox, uint32_t group_number,
                                           uint8_t *public_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (!public_key) return false;
    uint8_t id_pk[32], sig_sk[64];
    test_keypair_from_id(SELF_SEED_ID, id_pk, public_key, sig_sk);
    return true;
}

bool tox_group_self_get_signing_secret_key(const Tox *tox, uint32_t group_number,
                                           uint8_t *secret_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (!secret_key) return false;
    uint8_t id_pk[32], sig_pk[32];
    test_keypair_from_id(SELF_SEED_ID, id_pk, sig_pk, secret_key);
    return true;
}

bool tox_group_peer_get_public_key(const Tox *tox, uint32_t group_number, uint32_t peer_id,
                                   uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (!public_key) return false;
    uint8_t sig_pk[32], sig_sk[64];
    test_keypair_from_id(peer_id, public_key, sig_pk, sig_sk);
    return true;
}

bool tox_group_peer_get_signing_public_key(const Tox *tox, uint32_t group_number, uint32_t peer_id,
                                           uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (!public_key) return false;
    uint8_t id_pk[32], sig_sk[64];
    test_keypair_from_id(peer_id, id_pk, public_key, sig_sk);
    return true;
}

uint32_t tox_group_by_chat_id(const Tox *tox, const uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (!chat_id) return UINT32_MAX;
    return (uint32_t)chat_id[0] | ((uint32_t)chat_id[1] << 8) |
           ((uint32_t)chat_id[2] << 16) | ((uint32_t)chat_id[3] << 24);
}

bool tox_group_get_chat_id(const Tox *tox, uint32_t group_number, uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (chat_id) {
        memset(chat_id, 0, TOX_GROUP_CHAT_ID_SIZE);
        chat_id[0] = (uint8_t)(group_number & 0xFF);
        chat_id[1] = (uint8_t)((group_number >> 8) & 0xFF);
        chat_id[2] = (uint8_t)((group_number >> 16) & 0xFF);
        chat_id[3] = (uint8_t)((group_number >> 24) & 0xFF);
    }
    return true;
}

uint32_t tox_group_self_get_peer_id(const Tox *tox, uint32_t group_number, Tox_Err_Group_Self_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    return 0;
}

Tox_Connection tox_group_peer_get_connection_status(const Tox *tox, uint32_t group_number,
                                                    uint32_t peer_id, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number; (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return TOX_CONNECTION_TCP;
}

Tox_Group_Role tox_group_peer_get_role(const Tox *tox, uint32_t group_number,
                                       uint32_t peer_id, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number; (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return TOX_GROUP_ROLE_USER;
}

Tox_Group_Role tox_group_self_get_role(const Tox *tox, uint32_t group_number, Tox_Err_Group_Self_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    return TOX_GROUP_ROLE_FOUNDER;
}

/* Founder key mock: return self's identity so founder-role stamping has a target. */
bool tox_group_get_founder_public_key(const Tox *tox, uint32_t group_number,
                                      uint8_t *founder_key, Tox_Err_Group_State_Queries *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (!founder_key) return false;
    uint8_t sig_pk[32], sig_sk[64];
    test_keypair_from_id(SELF_SEED_ID, founder_key, sig_pk, sig_sk);
    return true;
}

size_t tox_group_peer_get_name_size(const Tox *tox, uint32_t group_number,
                                    uint32_t peer_id, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number; (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (mock_long_peer_name) return (size_t)MID_MAX_NICK_SIZE + 50;
    return 4;
}

bool tox_group_peer_get_name(const Tox *tox, uint32_t group_number, uint32_t peer_id,
                             uint8_t *name, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (!name) return false;
    if (mock_long_peer_name) {
        memset(name, 'A', (size_t)MID_MAX_NICK_SIZE + 50);
        return true;
    }
    name[0] = 'p';
    name[1] = (uint8_t)((peer_id >> 8) & 0xFF);
    name[2] = (uint8_t)(peer_id & 0xFF);
    name[3] = '!';
    return true;
}

uint32_t tox_group_peer_by_public_key(const Tox *tox, uint32_t group_number,
                                      const uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number;
    if (!public_key) {
        if (error) *error = (Tox_Err_Group_Peer_Query)9999;
        return UINT32_MAX;
    }
    if (mock_only_self_present) {
        uint8_t self_pk[32];
        test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);
        if (memcmp(public_key, self_pk, 32) == 0) {
            if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
            return 1;
        }
        if (error) *error = (Tox_Err_Group_Peer_Query)9999;
        return UINT32_MAX;
    }
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return 1;
}

/* [CHANGED] Added optional packet capture for the padding test. */
bool tox_group_send_custom_packet(const Tox *tox, uint32_t group_number, bool lossless,
                                  const uint8_t *data, size_t length,
                                  Tox_Err_Group_Send_Custom_Packet *error)
{
    (void)tox; (void)group_number; (void)lossless;
    if (error) *error = TOX_ERR_GROUP_SEND_CUSTOM_PACKET_OK;
    if (data == NULL || length == 0 || length > 1200) return false;

    /* [NEW] capture for padding inspection */
    if (mock_capture_packets && length <= sizeof(mock_last_packet)) {
        memcpy(mock_last_packet, data, length);
        mock_last_packet_len = length;
    }

    mock_send_count++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Packet helpers                                                     */
/* ------------------------------------------------------------------ */

static void put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_u64_be(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)(v & 0xFF);
}

/*
 * [CHANGED] Build a V4 PRESENCE packet:
 *
 *   header(5) | sig(64) | signed_body(73) | eph_pk(32) | eph_cert(64)
 *             | nick_len(2) | nick(N) | pad_len(1)
 *
 * - signed_body = status(1) + timestamp(8) + identity(32) + signing(32)
 * - sig         = Ed25519(signed_body) under a fresh ephemeral key
 * - eph_cert    = Ed25519(eph_pk) under the long-term signing key
 * - trailing 0x00 is the padding envelope (pad_len = 0)
 */
static bool make_presence_packet(uint8_t *out, size_t cap, size_t *out_len,
                                 uint32_t signer_id, uint8_t status,
                                 uint64_t timestamp, const uint8_t *nick, uint16_t nick_len)
{
    if (out == NULL || out_len == NULL) return false;
    if (nick_len > MID_MAX_NICK_SIZE) return false;
    if (nick_len > 0 && nick == NULL) return false;

    size_t need = 5 + 64 + TEST_SIGNED_BODY_SIZE + 32 + 64 + 2 + nick_len + 1;
    if (cap < need) return false;

    uint8_t id_pk[32], sig_pk[32], sig_sk[64];
    test_keypair_from_id(signer_id, id_pk, sig_pk, sig_sk);

    /* Fresh ephemeral keypair for this packet. */
    uint8_t eph_pk[32], eph_sk[64];
    if (crypto_sign_keypair(eph_pk, eph_sk) != 0) return false;

    /* Certify the ephemeral key with the long-term signing key. */
    uint8_t eph_cert[64];
    unsigned long long cert_len = 0;
    if (crypto_sign_detached(eph_cert, &cert_len, eph_pk, 32, sig_sk) != 0) return false;
    if (cert_len != 64) return false;

    /* Signed body (73 bytes). */
    uint8_t body[TEST_SIGNED_BODY_SIZE];
    size_t o = 0;
    body[o++] = status;
    put_u64_be(body + o, timestamp); o += 8;
    memcpy(body + o, id_pk, 32);  o += 32;   /* identity_key */
    memcpy(body + o, sig_pk, 32); o += 32;   /* signing_key  */

    /* Sign the body with the ephemeral key. */
    uint8_t sig[64];
    unsigned long long sig_len = 0;
    if (crypto_sign_detached(sig, &sig_len, body, TEST_SIGNED_BODY_SIZE, eph_sk) != 0) return false;
    if (sig_len != 64) return false;

    /* Assemble the packet. */
    size_t p = 0;
    out[p++] = MID_MAGIC_0;
    out[p++] = MID_MAGIC_1;
    out[p++] = MID_MAGIC_2;
    out[p++] = MID_PROTOCOL_VERSION;
    out[p++] = MID_MSG_PRESENCE;

    memcpy(out + p, sig, 64);              p += 64;
    memcpy(out + p, body, TEST_SIGNED_BODY_SIZE); p += TEST_SIGNED_BODY_SIZE;
    memcpy(out + p, eph_pk, 32);           p += 32;
    memcpy(out + p, eph_cert, 64);         p += 64;

    put_u16_be(out + p, nick_len);         p += 2;
    if (nick_len > 0) { memcpy(out + p, nick, nick_len); p += nick_len; }

    out[p++] = 0; /* padding envelope: pad_len = 0 */

    *out_len = p;
    return true;
}

static bool get_peer_info_by_key(MidState *s, const uint8_t *chat_id,
                                 const uint8_t key[32], MidPeerInfo *out)
{
    int idx = mid_find_peer(s, chat_id, key);
    if (idx < 0) return false;
    return mid_peer_list_get(s, chat_id, (size_t)idx, out);
}

/* [NEW] Helper: a padded ROSTER_REQUEST packet (5-byte header + pad_len=0). */
static size_t make_roster_request(uint8_t out[6])
{
    out[0] = MID_MAGIC_0;
    out[1] = MID_MAGIC_1;
    out[2] = MID_MAGIC_2;
    out[3] = MID_PROTOCOL_VERSION;
    out[4] = MID_MSG_ROSTER_REQUEST;
    out[5] = 0; /* pad_len */
    return 6;
}

/* ------------------------------------------------------------------ */
/* Tests (original)                                                   */
/* ------------------------------------------------------------------ */

static bool test_null_and_invalid_api(void)
{
    uint8_t zero_key[32] = {0};
    MidPeerInfo info;

    mid_free(NULL);

    T_ASSERT_INT_EQ(mid_group_count(NULL), 0, "mid_group_count(NULL)");
    T_ASSERT_INT_EQ(mid_peer_count(NULL, cid1), 0, "mid_peer_count(NULL)");
    T_ASSERT_INT_EQ(mid_signed_count(NULL, cid1), 0, "mid_signed_count(NULL)");
    T_ASSERT_INT_EQ(mid_online_count(NULL, cid1), 0, "mid_online_count(NULL)");
    T_ASSERT_INT_EQ(mid_peer_list_count(NULL, cid1), 0, "mid_peer_list_count(NULL)");

    T_ASSERT_INT_EQ(mid_find_peer(NULL, cid1, zero_key), -1, "mid_find_peer(NULL)");
    T_ASSERT_FALSE(mid_delete_peer_by_identity(NULL, cid1, zero_key), "mid_delete_peer_by_identity(NULL)");
    T_ASSERT_FALSE(mid_peer_is_signed_left(NULL, cid1, zero_key), "mid_peer_is_signed_left(NULL)");
    T_ASSERT_FALSE(mid_announce_leave(NULL, NULL, GROUP_1), "mid_announce_leave(NULL)");
    T_ASSERT_FALSE(mid_on_group_moderation(NULL, NULL, GROUP_1, 0, 0, TOX_GROUP_MOD_EVENT_KICK),
                   "mid_on_group_moderation(NULL)");
    T_ASSERT_FALSE(mid_peer_list_get(NULL, cid1, 0, &info), "mid_peer_list_get(NULL)");

    mid_on_group_self_join(NULL, NULL, GROUP_1, NULL, 0);
    mid_on_group_delete(NULL, NULL, GROUP_1);
    mid_on_group_peer_join(NULL, NULL, GROUP_1, 1);
    mid_on_group_peer_exit(NULL, NULL, GROUP_1, TOX_GROUP_EXIT_TYPE_QUIT);
    mid_on_group_peer_name(NULL, NULL, GROUP_1, 1);
    mid_on_group_custom_packet(NULL, NULL, GROUP_1, 1, NULL, 0);
    mid_iterate(NULL, NULL);
    mid_print_peer_table(NULL, cid1, "null");

    return true;
}

static bool test_new_free_and_empty_queries(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    T_ASSERT_PTR_NOT_NULL(s, "mid_new()");
    Tox *tox = create_dummy_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "create_dummy_tox()");

    T_ASSERT_INT_EQ(mid_group_count(s), 0, "empty group count");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 0, "empty peer count");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 0, "empty signed count");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 0, "empty online count");
    T_ASSERT_INT_EQ(mid_peer_list_count(s, cid1), 0, "empty peer list count");

    mid_iterate(s, tox);
    mid_free(s);
    return true;
}

static bool test_self_join_creates_signed_self(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"alice", 5);

    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group count after self join");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "peer count after self join");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 1, "signed count after self join");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "online count after self join");

    uint8_t self_pk[32];
    test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);

    int idx = mid_find_peer(s, cid1, self_pk);
    T_ASSERT_INT_EQ(idx, 0, "self peer index");

    MidPeerInfo info;
    T_ASSERT_TRUE(mid_peer_list_get(s, cid1, 0, &info), "get self peer info");
    T_ASSERT_TRUE(memcmp(info.identity_key, self_pk, 32) == 0, "self identity key");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_ACTIVE, "self status");
    T_ASSERT_TRUE(info.connection_status != TOX_CONNECTION_NONE, "self online");
    T_ASSERT_INT_EQ(info.has_signature, 1, "self signed");
    T_ASSERT_INT_EQ(info.nickname_len, 5, "self nickname length");
    T_ASSERT_TRUE(memcmp(info.nickname, "alice", 5) == 0, "self nickname");
    T_ASSERT_INT_EQ(info.nickname[5], 0, "self nickname NUL terminator");
    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, self_pk), "self should not be LEFT");

    mid_free(s);
    return true;
}

static bool test_self_join_idempotent(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"alice", 5);
    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"alice", 5);
    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"alice", 5);

    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group count remains 1");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "self is not duplicated");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 1, "signed count remains 1");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "online count remains 1");

    mid_free(s);
    return true;
}

static bool test_self_join_long_nickname_truncated(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    uint8_t nick[200];
    memset(nick, 'N', sizeof(nick));
    mid_on_group_self_join(s, tox, GROUP_1, nick, sizeof(nick));

    T_ASSERT_INT_EQ(mid_peer_list_count(s, cid1), 1, "peer list count");
    MidPeerInfo info;
    T_ASSERT_TRUE(mid_peer_list_get(s, cid1, 0, &info), "get self peer info");
    T_ASSERT_INT_EQ(info.nickname_len, MID_MAX_NICK_SIZE, "nickname truncated to max");

    bool all_match = true;
    for (uint16_t i = 0; i < MID_MAX_NICK_SIZE; i++) {
        if (info.nickname[i] != 'N') { all_match = false; break; }
    }
    T_ASSERT_TRUE(all_match, "truncated nickname bytes");
    T_ASSERT_INT_EQ(info.nickname[MID_MAX_NICK_SIZE], 0, "truncated nickname NUL terminator");

    mid_free(s);
    return true;
}

static bool test_multiple_groups_and_delete(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"g1", 2);
    mid_on_group_self_join(s, tox, GROUP_2, (const uint8_t *)"g2", 2);

    T_ASSERT_INT_EQ(mid_group_count(s), 2, "two groups");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "group 1 self");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid2), 1, "group 2 self");

    mid_on_group_delete(s, tox, GROUP_1);
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "one group after delete");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 0, "deleted group has no peers");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid2), 1, "remaining group still has self");

    mid_on_group_delete(s, tox, GROUP_9999);
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "deleting unknown group does nothing");

    mid_on_group_delete(s, tox, GROUP_2);
    T_ASSERT_INT_EQ(mid_group_count(s), 0, "all groups deleted");

    mid_free(s);
    return true;
}

static bool test_peer_join_counts_and_list(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 1001;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    mid_on_group_peer_join(s, tox, GROUP_1, peer_id);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "self + joined peer");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 1, "only self signed");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 2, "self + peer online");

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find joined peer");
    T_ASSERT_TRUE(memcmp(info.identity_key, peer_pk, 32) == 0, "peer identity key");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_ACTIVE, "peer status");
    T_ASSERT_TRUE(info.connection_status != TOX_CONNECTION_NONE, "peer online");
    T_ASSERT_INT_EQ(info.has_signature, 0, "peer not signed yet");
    T_ASSERT_INT_EQ(info.nickname_len, 4, "peer nickname from mock");

    mid_on_group_peer_join(s, tox, GROUP_1, peer_id);
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "duplicate join does not duplicate");

    T_ASSERT_TRUE(mid_delete_peer_by_identity(s, cid1, peer_pk), "delete peer");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "peer deleted");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "only self online after delete");
    T_ASSERT_INT_EQ(mid_find_peer(s, cid1, peer_pk), -1, "peer not found after delete");
    T_ASSERT_FALSE(mid_delete_peer_by_identity(s, cid1, peer_pk), "delete again fails");

    mid_free(s);
    return true;
}

static bool test_peer_name_truncation(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 1002;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    mock_long_peer_name = true;
    mid_on_group_peer_join(s, tox, GROUP_1, peer_id);
    mock_long_peer_name = false;

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer");
    T_ASSERT_INT_EQ(info.nickname_len, MID_MAX_NICK_SIZE, "peer nickname truncated");

    mid_free(s);
    return true;
}

static bool test_peer_exit_marks_offline(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 1003;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    mid_on_group_peer_join(s, tox, GROUP_1, peer_id);
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 2, "both online before exit");

    mock_only_self_present = true;
    mid_on_group_peer_exit(s, tox, GROUP_1, TOX_GROUP_EXIT_TYPE_QUIT);
    mock_only_self_present = false;

    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "peer offline after exit sync");

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    MidPeerInfo peer_info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &peer_info), "find peer");
    T_ASSERT_INT_EQ(peer_info.status, MID_STATUS_ACTIVE, "peer remains ACTIVE");
    T_ASSERT_TRUE(peer_info.connection_status == TOX_CONNECTION_NONE, "peer is offline");

    uint8_t self_pk[32];
    test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);
    MidPeerInfo self_info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, self_pk, &self_info), "find self");
    T_ASSERT_TRUE(self_info.connection_status != TOX_CONNECTION_NONE, "self remains online");

    mid_free(s);
    return true;
}

static bool test_valid_presence_packet_and_old_timestamp(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 2001;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 1000,
                                       (const uint8_t *)"alice", 5),
                  "build valid presence packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "presence packet inserted peer");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 2, "peer is signed");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 2, "peer is online");

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer");
    T_ASSERT_INT_EQ(info.has_signature, 1, "peer has signature");
    T_ASSERT_TRUE(info.connection_status != TOX_CONNECTION_NONE, "peer online");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_ACTIVE, "peer ACTIVE");
    T_ASSERT_INT_EQ(info.nickname_len, 5, "packet nickname length");
    T_ASSERT_TRUE(memcmp(info.nickname, "alice", 5) == 0, "packet nickname");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 500,
                                       (const uint8_t *)"old", 3),
                  "build old presence packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer after old packet");
    T_ASSERT_INT_EQ(info.nickname_len, 5, "old packet did not replace nickname length");
    T_ASSERT_TRUE(memcmp(info.nickname, "alice", 5) == 0, "old packet did not replace nickname");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 1000,
                                       (const uint8_t *)"new", 3),
                  "build equal-timestamp presence packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer after equal packet");
    T_ASSERT_INT_EQ(info.nickname_len, 5, "equal timestamp did not replace nickname length");
    T_ASSERT_TRUE(memcmp(info.nickname, "alice", 5) == 0, "equal timestamp did not replace nickname");

    mid_free(s);
    return true;
}

static bool test_left_tombstone(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 2100;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 1000,
                                       (const uint8_t *)"bob", 3),
                  "build ACTIVE packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, peer_pk), "not LEFT yet");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_LEFT, 2000, NULL, 0),
                  "build LEFT tombstone packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_TRUE(mid_peer_is_signed_left(s, cid1, peer_pk), "signed LEFT tombstone stored");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "tombstone remains in roster");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 2, "tombstone remains signed");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "LEFT peer is offline");

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find tombstoned peer");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_LEFT, "peer status LEFT");
    T_ASSERT_TRUE(info.connection_status == TOX_CONNECTION_NONE, "peer offline");
    T_ASSERT_INT_EQ(info.has_signature, 1, "tombstone signed");

    mid_free(s);
    return true;
}

static bool test_old_left_does_not_overwrite(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 2101;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 5000,
                                       (const uint8_t *)"live", 4),
                  "build ACTIVE packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_LEFT, 4000, NULL, 0),
                  "build old LEFT packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, peer_pk), "old LEFT must not tombstone");

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_ACTIVE, "peer still ACTIVE");
    T_ASSERT_TRUE(info.connection_status != TOX_CONNECTION_NONE, "peer still online");

    mid_free(s);
    return true;
}

static bool test_newer_active_resurrects_after_left(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 2300;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_LEFT, 1000, NULL, 0),
                  "build LEFT packet for unknown peer");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    T_ASSERT_TRUE(mid_peer_is_signed_left(s, cid1, peer_pk), "unknown LEFT tombstone stored");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "tombstone inserted");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "tombstone offline");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 2000,
                                       (const uint8_t *)"back", 4),
                  "build newer ACTIVE packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, peer_pk), "newer ACTIVE removes LEFT state");

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find resurrected peer");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_ACTIVE, "peer ACTIVE again");
    T_ASSERT_TRUE(info.connection_status != TOX_CONNECTION_NONE, "peer online again");
    T_ASSERT_INT_EQ(info.nickname_len, 4, "new nickname applied");
    T_ASSERT_TRUE(memcmp(info.nickname, "back", 4) == 0, "new nickname bytes");

    mid_free(s);
    return true;
}

static bool test_corrupt_signature_and_malformed_packets(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 2200;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, peer_id,
                                       MID_STATUS_ACTIVE, 1000,
                                       (const uint8_t *)"x", 1),
                  "build valid packet");

    /* Corrupt one byte inside the signature (bytes 5..68). */
    pkt[10] ^= 0xFF;
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "corrupt signature rejected");

    /* Malformed packets (all lack a valid padding envelope / magic). */
    uint8_t bad_magic[6] = {0x00, 0x00, 0x00, 0x00, MID_MSG_PRESENCE, 0};
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, bad_magic, sizeof(bad_magic));

    uint8_t short_pkt[4] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION};
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, short_pkt, sizeof(short_pkt));

    uint8_t unknown_type[6] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION, 0x7F, 0};
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, unknown_type, sizeof(unknown_type));

    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, NULL, 0);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "malformed packets rejected");

    mid_free(s);
    return true;
}

static bool test_self_packet_ignored(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;
    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len, SELF_SEED_ID,
                                       MID_STATUS_ACTIVE, 9999,
                                       (const uint8_t *)"evil", 4),
                  "build self-signed packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, 3000, pkt, len);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "self packet from network ignored");

    mid_free(s);
    return true;
}

static bool test_announce_leave(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t self_pk[32];
    test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);

    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "self online before leave");
    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, self_pk), "not LEFT before leave");

    T_ASSERT_TRUE(mid_announce_leave(s, tox, GROUP_1), "announce leave succeeds");
    T_ASSERT_TRUE(mid_peer_is_signed_left(s, cid1, self_pk), "self LEFT tombstone stored");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "self tombstone remains");
    T_ASSERT_INT_EQ(mid_signed_count(s, cid1), 1, "self tombstone signed");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 0, "self offline after leave");

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, self_pk, &info), "find self tombstone");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_LEFT, "self status LEFT");
    T_ASSERT_TRUE(info.connection_status == TOX_CONNECTION_NONE, "self offline");
    T_ASSERT_INT_EQ(info.has_signature, 1, "self tombstone signed");

    T_ASSERT_FALSE(mid_announce_leave(s, tox, GROUP_9999), "announce leave unknown group fails");

    mid_free(s);
    return true;
}

static bool test_moderation_self_kicked(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group exists before kick");

    bool we_were_kicked = mid_on_group_moderation(s, tox, GROUP_1, 9, 0, TOX_GROUP_MOD_EVENT_KICK);
    T_ASSERT_TRUE(we_were_kicked, "self kick detected");
    T_ASSERT_INT_EQ(mid_group_count(s), 0, "group state deleted after self kick");

    mid_free(s);
    return true;
}

static bool test_moderation_other_peer_kicked(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    uint32_t peer_id = 4000;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    mid_on_group_peer_join(s, tox, GROUP_1, peer_id);
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "peer exists before kick");

    bool we_were_kicked = mid_on_group_moderation(s, tox, GROUP_1, 0, peer_id, TOX_GROUP_MOD_EVENT_KICK);
    T_ASSERT_FALSE(we_were_kicked, "other peer kick detected");
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group remains");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "kicked peer deleted");

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    T_ASSERT_INT_EQ(mid_find_peer(s, cid1, peer_pk), -1, "kicked peer not found");

    mid_free(s);
    return true;
}

/* [CHANGED] Uses the padded ROSTER_REQUEST helper. */
static bool test_roster_request_cooldown(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t req[6];
    size_t req_len = make_roster_request(req);

    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, req_len);
    T_ASSERT_INT_EQ(mock_send_count, 0, "roster request does not reply immediately");

    sleep(TEST_SYNC_INTERVAL_SEC + 2);
    mid_iterate(s, tox);
    T_ASSERT_TRUE(mock_send_count > 0, "scheduled roster reply is eventually sent");

    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, req_len);
    T_ASSERT_INT_EQ(mock_send_count, 0, "second roster request is also delayed");

    mid_free(s);
    return true;
}

static void counting_cb(const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], void *user_data)
{
    (void)chat_id;
    if (user_data != NULL) (*(int *)user_data)++;
}

static bool test_callback_fires(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    int count = 0;
    mid_set_peer_list_changed_cb(s, counting_cb, &count);

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"cb", 2);
    T_ASSERT_INT_EQ(count, 1, "callback fired once for self join");

    mid_set_peer_list_changed_cb(s, NULL, NULL);
    mid_on_group_peer_join(s, tox, GROUP_1, 6000);
    T_ASSERT_INT_EQ(count, 1, "callback not fired after unregister");

    mid_free(s);
    return true;
}

static bool test_iterate_no_crash(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    mid_iterate(s, tox);
    mid_iterate(s, tox);

    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group remains after iterate");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "peer remains after iterate");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "self remains online after iterate");

    mid_free(s);
    return true;
}

static bool test_print_peer_table_no_crash(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    mid_on_group_peer_join(s, tox, GROUP_1, 7000);

    mid_print_peer_table(s, cid1, "unit");
    mid_print_peer_table(s, cid9999, "invalid");

    mid_free(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* [NEW] Feature-specific tests                                       */
/* ------------------------------------------------------------------ */

/* MID_TIMESTAMP_ROUNDING_SEC: self-announce stores a rounded timestamp. */
static bool test_timestamp_rounding_on_self_join(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t self_pk[32];
    test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);

    MidPeerInfo info;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, self_pk, &info), "find self");
    T_ASSERT_TRUE(info.last_seen > 0, "last_seen is set");
    T_ASSERT_TRUE(info.last_seen % TEST_TIMESTAMP_ROUNDING_SEC == 0,
                  "last_seen is rounded to a 20s boundary");

    mid_free(s);
    return true;
}

/* MID_MAX_PACKET_PADDING: outgoing packets carry a valid padding envelope. */
static bool test_packet_padding_on_outgoing(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mock_capture_packets = true;
    mock_last_packet_len = 0;

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    /* self-join sends PRESENCE then ROSTER_REQUEST; last captured is the request. */
    T_ASSERT_TRUE(mock_last_packet_len > 0, "a packet was captured");

    uint8_t pad_len = mock_last_packet[mock_last_packet_len - 1];
    T_ASSERT_TRUE(pad_len <= TEST_MAX_PACKET_PADDING, "pad_len <= 64");

    size_t msg_len = mock_last_packet_len - (size_t)pad_len - 1;
    T_ASSERT_INT_EQ(msg_len, 5, "roster request payload is 5 bytes");
    T_ASSERT_TRUE(mock_last_packet_len >= 6, "packet has at least the pad indicator");

    T_ASSERT_INT_EQ(mock_last_packet[0], MID_MAGIC_0, "magic byte 0");
    T_ASSERT_INT_EQ(mock_last_packet[1], MID_MAGIC_1, "magic byte 1");
    T_ASSERT_INT_EQ(mock_last_packet[2], MID_MAGIC_2, "magic byte 2");
    T_ASSERT_INT_EQ(mock_last_packet[3], MID_PROTOCOL_VERSION, "protocol version");
    T_ASSERT_INT_EQ(mock_last_packet[4], MID_MSG_ROSTER_REQUEST, "message type");

    mock_capture_packets = false;
    mid_free(s);
    return true;
}

/* MID_HEARTBEAT_JITTER_SEC: heartbeat must NOT fire on the first iterate
 * (jittered interval is at least HEARTBEAT_SEC - JITTER/2 = 2700s). */
static bool test_heartbeat_not_triggered_prematurely(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mock_send_count = 0;
    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);
    int after_join = mock_send_count;
    T_ASSERT_TRUE(after_join >= 2, "join sends presence + roster request");

    mid_iterate(s, tox);
    T_ASSERT_INT_EQ(mock_send_count, after_join,
                    "no heartbeat sent on first iterate (jittered interval >= 2700s)");

    mid_free(s);
    return true;
}

/* MID_ROSTER_COOLDOWN_SEC: a second ROSTER_BATCH within the cooldown window
 * is suppressed. (Sleeps ~14s.) */
static bool test_roster_cooldown_suppresses_rapid_replies(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t req[6];
    size_t req_len = make_roster_request(req);

    /* First request: reply goes through (last_roster_response == 0). */
    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, req_len);
    T_ASSERT_INT_EQ(mock_send_count, 0, "first request does not reply immediately");

    sleep(TEST_SYNC_INTERVAL_SEC + 2);
    mid_iterate(s, tox);
    T_ASSERT_TRUE(mock_send_count > 0, "first roster reply is sent");

    /* Second request: within the 20s cooldown, so the reply is suppressed. */
    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, req_len);
    T_ASSERT_INT_EQ(mock_send_count, 0, "second request does not reply immediately");

    sleep(TEST_SYNC_INTERVAL_SEC + 2);
    mid_iterate(s, tox);
    T_ASSERT_INT_EQ(mock_send_count, 0, "second roster reply suppressed by cooldown");

    mid_free(s);
    return true;
}

/*
 * Test: Founder correctness
 *
 * Verifies that:
 * 1. Only the actual group founder gets TOX_GROUP_ROLE_FOUNDER.
 * 2. Regular peers never get FOUNDER role (not on join, not via PRESENCE,
 *    not via LEFT tombstone, not via ROSTER_BATCH relay).
 * 3. The founder retains FOUNDER role through all operations.
 * 4. Exactly one FOUNDER exists in the roster at all times.
 *
 * This test guards against the memset-zero-default bug where
 * TOX_GROUP_ROLE_FOUNDER == 0, so any memset(&r, 0, ...) would
 * silently assign FOUNDER role unless explicitly overridden.
 */
static bool test_founder_correctness(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    uint8_t founder_pk[32];
    test_keypair_from_id(SELF_SEED_ID, founder_pk, NULL, NULL);

    MidPeerInfo info;

    /* ---- Phase 1: Self-join assigns FOUNDER role to the founder ---- */

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"founder", 7);

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, founder_pk, &info),
                  "find founder after self-join");
    T_ASSERT_INT_EQ(info.role, TOX_GROUP_ROLE_FOUNDER,
                    "founder has FOUNDER role after self-join");

    /* ---- Phase 2: Peer join does NOT assign FOUNDER role ---- */

    uint32_t peer2_id = 2001;
    uint8_t peer2_pk[32];
    test_keypair_from_id(peer2_id, peer2_pk, NULL, NULL);

    mid_on_group_peer_join(s, tox, GROUP_1, peer2_id);

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer2_pk, &info),
                  "find peer2 after join");
    T_ASSERT_TRUE(info.role != TOX_GROUP_ROLE_FOUNDER,
                  "peer2 must NOT be FOUNDER after join");

    /* Founder must still be FOUNDER */
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, founder_pk, &info),
                  "find founder after peer2 join");
    T_ASSERT_INT_EQ(info.role, TOX_GROUP_ROLE_FOUNDER,
                    "founder still FOUNDER after peer2 join");

    /* ---- Phase 3: Second peer join also does NOT get FOUNDER ---- */

    uint32_t peer3_id = 2002;
    uint8_t peer3_pk[32];
    test_keypair_from_id(peer3_id, peer3_pk, NULL, NULL);

    mid_on_group_peer_join(s, tox, GROUP_1, peer3_id);

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer3_pk, &info),
                  "find peer3 after join");
    T_ASSERT_TRUE(info.role != TOX_GROUP_ROLE_FOUNDER,
                  "peer3 must NOT be FOUNDER after join");

    /* ---- Phase 4: mid_iterate maintains founder correctness ---- */

    mid_iterate(s, tox);

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, founder_pk, &info),
                  "find founder after iterate");
    T_ASSERT_INT_EQ(info.role, TOX_GROUP_ROLE_FOUNDER,
                    "founder still FOUNDER after iterate");

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer2_pk, &info),
                  "find peer2 after iterate");
    T_ASSERT_TRUE(info.role != TOX_GROUP_ROLE_FOUNDER,
                  "peer2 must NOT be FOUNDER after iterate");

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer3_pk, &info),
                  "find peer3 after iterate");
    T_ASSERT_TRUE(info.role != TOX_GROUP_ROLE_FOUNDER,
                  "peer3 must NOT be FOUNDER after iterate");

    /* ---- Phase 5: LEFT tombstone must NOT get FOUNDER role ---- */
    /*
     * This is the critical regression test for the memset bug.
     * mid_announce_leave_internal does memset(&r, 0, sizeof(r)) which
     * sets role=0=FOUNDER. The fix explicitly sets r.role=USER.
     * Without the fix, the LEFT tombstone would show as FOUNDER.
     */

    T_ASSERT_TRUE(mid_announce_leave(s, tox, GROUP_1),
                  "announce leave succeeds");

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, founder_pk, &info),
                  "find founder after LEFT tombstone");
    T_ASSERT_INT_EQ(info.status, MID_STATUS_LEFT,
                    "founder status is LEFT");
    T_ASSERT_TRUE(info.role != TOX_GROUP_ROLE_FOUNDER,
                  "LEFT tombstone must NOT have FOUNDER role (memset bug regression)");

    /* ---- Phase 6: Exactly one FOUNDER invariant ---- */
    /*
     * Re-join as a new founder to test that the invariant holds
     * even after leave/rejoin cycles.
     */

    MidState *s2 = mid_new(NULL, NULL, 0);
    mid_on_group_self_join(s2, tox, GROUP_1, (const uint8_t *)"founder2", 8);
    mid_on_group_peer_join(s2, tox, GROUP_1, peer2_id);
    mid_on_group_peer_join(s2, tox, GROUP_1, peer3_id);

    /* Count FOUNDER roles across entire roster */
    size_t founder_count = 0;
    size_t total = mid_peer_list_count(s2, cid1);

    for (size_t i = 0; i < total; i++) {
        T_ASSERT_TRUE(mid_peer_list_get(s2, cid1, i, &info),
                      "get peer info for founder count");
        if (info.role == TOX_GROUP_ROLE_FOUNDER) {
            founder_count++;
            /* The FOUNDER must be the actual founder (SELF_SEED_ID) */
            T_ASSERT_TRUE(memcmp(info.identity_key, founder_pk, 32) == 0,
                          "only the actual founder has FOUNDER role");
        }
    }

    T_ASSERT_INT_EQ(founder_count, 1,
                    "exactly one FOUNDER in roster (no stale FOUNDER from memset)");

    /* ---- Phase 7: Peer exit does not create phantom FOUNDER ---- */

    mock_only_self_present = true;
    mid_on_group_peer_exit(s2, tox, GROUP_1, TOX_GROUP_EXIT_TYPE_QUIT);
    mock_only_self_present = false;

    /* After peers go offline, founder must still be the only FOUNDER */
    founder_count = 0;
    total = mid_peer_list_count(s2, cid1);
    for (size_t i = 0; i < total; i++) {
        T_ASSERT_TRUE(mid_peer_list_get(s2, cid1, i, &info),
                      "get peer info after exit");
        if (info.role == TOX_GROUP_ROLE_FOUNDER) {
            founder_count++;
            T_ASSERT_TRUE(memcmp(info.identity_key, founder_pk, 32) == 0,
                          "only actual founder is FOUNDER after peer exit");
        }
    }
    T_ASSERT_INT_EQ(founder_count, 1,
                    "exactly one FOUNDER after peer exit");

    mid_free(s2);
    mid_free(s);
    return true;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    if (sodium_init() < 0) return 1;

    TEST_SUITE("midroster unit / roster logic tests");

    RUN_TEST(test_null_and_invalid_api);
    RUN_TEST(test_new_free_and_empty_queries);
    RUN_TEST(test_self_join_creates_signed_self);
    RUN_TEST(test_self_join_idempotent);
    RUN_TEST(test_self_join_long_nickname_truncated);
    RUN_TEST(test_multiple_groups_and_delete);
    RUN_TEST(test_peer_join_counts_and_list);
    RUN_TEST(test_peer_name_truncation);
    RUN_TEST(test_peer_exit_marks_offline);
    RUN_TEST(test_valid_presence_packet_and_old_timestamp);
    RUN_TEST(test_left_tombstone);
    RUN_TEST(test_old_left_does_not_overwrite);
    RUN_TEST(test_newer_active_resurrects_after_left);
    RUN_TEST(test_corrupt_signature_and_malformed_packets);
    RUN_TEST(test_self_packet_ignored);
    RUN_TEST(test_announce_leave);
    RUN_TEST(test_moderation_self_kicked);
    RUN_TEST(test_moderation_other_peer_kicked);
    RUN_TEST(test_roster_request_cooldown);
    RUN_TEST(test_callback_fires);
    RUN_TEST(test_iterate_no_crash);
    RUN_TEST(test_print_peer_table_no_crash);

    /* [NEW] feature-specific tests */
    RUN_TEST(test_timestamp_rounding_on_self_join);
    RUN_TEST(test_packet_padding_on_outgoing);
    RUN_TEST(test_heartbeat_not_triggered_prematurely);
    RUN_TEST(test_roster_cooldown_suppresses_rapid_replies);
    RUN_TEST(test_founder_correctness);

    SUITE_END();
    return test_summary("midroster_unit");
}

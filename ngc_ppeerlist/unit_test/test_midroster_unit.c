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

#define MID_STATUS_ACTIVE      0
#define MID_STATUS_LEFT        1

#define SELF_SEED_ID           0x77777777u

#define TEST_BODY_MAX          (1 + 8 + 2 + MID_MAX_NICK_SIZE + 32 + 32)

/* ------------------------------------------------------------------ */
/* Group numbers and Chat ID constants                                */
/* ------------------------------------------------------------------ */

#define GROUP_1    1
#define GROUP_2    2
#define GROUP_9999 9999

static const uint8_t cid1[TOX_GROUP_CHAT_ID_SIZE] = {1};
static const uint8_t cid2[TOX_GROUP_CHAT_ID_SIZE] = {2};
static const uint8_t cid9999[TOX_GROUP_CHAT_ID_SIZE] = {0x0F, 0x27}; /* 9999 in LE */

/* ------------------------------------------------------------------ */
/* Mock state                                                         */
/* ------------------------------------------------------------------ */

static bool mock_only_self_present = false;
static bool mock_long_peer_name    = false;
static int  mock_send_count        = 0;

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

/* 
 * Updated to generate both the Curve25519 identity key and the 
 * Ed25519 signing key, satisfying mid_valid_key_binding().
 */
static void test_keypair_from_id(uint32_t id, uint8_t id_pk[32], uint8_t sig_pk[32], uint8_t sig_sk[64])
{
    uint8_t seed[32];
    uint8_t tmp_pk[32];
    uint8_t tmp_sk[64];

    test_seed_from_u32(id, seed);
    crypto_sign_seed_keypair(tmp_pk, tmp_sk, seed);

    if (sig_pk != NULL) {
        memcpy(sig_pk, tmp_pk, 32);
    }
    if (sig_sk != NULL) {
        memcpy(sig_sk, tmp_sk, 64);
    }
    if (id_pk != NULL) {
        /* Derive Curve25519 identity key from Ed25519 signing key */
        if (crypto_sign_ed25519_pk_to_curve25519(id_pk, tmp_pk) != 0) {
            memcpy(id_pk, tmp_pk, 32); /* Fallback, should never happen */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Tox mocks                                                          */
/* ------------------------------------------------------------------ */

typedef struct DummyTox {
    int dummy;
} DummyTox;

Tox *create_dummy_tox(void)
{
    static DummyTox t;
    return (Tox *)&t;
}



bool tox_group_self_get_public_key(const Tox *tox,
                                   uint32_t group_number,
                                   uint8_t *public_key,
                                   Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    }
    if (public_key == NULL) {
        return false;
    }

    uint8_t sig_pk[32], sig_sk[64];
    test_keypair_from_id(SELF_SEED_ID, public_key, sig_pk, sig_sk);
    return true;
}

bool tox_group_self_get_signing_public_key(const Tox *tox,
                                           uint32_t group_number,
                                           uint8_t *public_key,
                                           Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    }
    if (public_key == NULL) {
        return false;
    }

    uint8_t id_pk[32], sig_sk[64];
    test_keypair_from_id(SELF_SEED_ID, id_pk, public_key, sig_sk);
    return true;
}

bool tox_group_self_get_signing_secret_key(const Tox *tox,
                                           uint32_t group_number,
                                           uint8_t *secret_key,
                                           Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    }
    if (secret_key == NULL) {
        return false;
    }

    uint8_t id_pk[32], sig_pk[32];
    test_keypair_from_id(SELF_SEED_ID, id_pk, sig_pk, secret_key);
    return true;
}

bool tox_group_peer_get_public_key(const Tox *tox,
                                   uint32_t group_number,
                                   uint32_t peer_id,
                                   uint8_t *public_key,
                                   Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    if (public_key == NULL) {
        return false;
    }

    uint8_t sig_pk[32], sig_sk[64];
    test_keypair_from_id(peer_id, public_key, sig_pk, sig_sk);
    return true;
}

bool tox_group_peer_get_signing_public_key(const Tox *tox,
                                           uint32_t group_number,
                                           uint32_t peer_id,
                                           uint8_t *public_key,
                                           Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    if (public_key == NULL) {
        return false;
    }

    uint8_t id_pk[32], sig_sk[64];
    test_keypair_from_id(peer_id, id_pk, public_key, sig_sk);
    return true;
}





uint32_t tox_group_by_chat_id(const Tox *tox, const uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error != NULL) {
        *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    }
    if (chat_id == NULL) {
        return UINT32_MAX;
    }
    /* Reconstruct the group_number from the first 4 bytes (little-endian) */
    return (uint32_t)chat_id[0] |
           ((uint32_t)chat_id[1] << 8) |
           ((uint32_t)chat_id[2] << 16) |
           ((uint32_t)chat_id[3] << 24);
}

bool tox_group_get_chat_id(const Tox *tox, uint32_t group_number, uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error != NULL) {
        *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    }
    if (chat_id != NULL) {
        memset(chat_id, 0, TOX_GROUP_CHAT_ID_SIZE);
        chat_id[0] = (uint8_t)(group_number & 0xFF);
        chat_id[1] = (uint8_t)((group_number >> 8) & 0xFF);
        chat_id[2] = (uint8_t)((group_number >> 16) & 0xFF);
        chat_id[3] = (uint8_t)((group_number >> 24) & 0xFF);
    }
    return true;
}




uint32_t tox_group_self_get_peer_id(const Tox *tox,
                                    uint32_t group_number,
                                    Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    }
    return 0;
}

/*
 * Connection / role mocks.
 *
 * These were missing, so the weak defaults returned TOX_CONNECTION_NONE and
 * every peer looked offline. Returning TCP makes self-join and peer-join mark
 * records online, which is what the assertions expect.
 */
Tox_Connection tox_group_peer_get_connection_status(const Tox *tox,
                                                    uint32_t group_number,
                                                    uint32_t peer_id,
                                                    Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    return TOX_CONNECTION_TCP;
}

Tox_Group_Role tox_group_peer_get_role(const Tox *tox,
                                       uint32_t group_number,
                                       uint32_t peer_id,
                                       Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    return TOX_GROUP_ROLE_USER;
}

Tox_Group_Role tox_group_self_get_role(const Tox *tox,
                                       uint32_t group_number,
                                       Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    }
    return TOX_GROUP_ROLE_FOUNDER;
}

size_t tox_group_peer_get_name_size(const Tox *tox,
                                    uint32_t group_number,
                                    uint32_t peer_id,
                                    Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }

    if (mock_long_peer_name) {
        return (size_t)MID_MAX_NICK_SIZE + 50;
    }
    return 4;
}

bool tox_group_peer_get_name(const Tox *tox,
                             uint32_t group_number,
                             uint32_t peer_id,
                             uint8_t *name,
                             Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    if (name == NULL) {
        return false;
    }

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

uint32_t tox_group_peer_by_public_key(const Tox *tox,
                                      uint32_t group_number,
                                      const uint8_t *public_key,
                                      Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;

    if (public_key == NULL) {
        if (error != NULL) {
            *error = (Tox_Err_Group_Peer_Query)9999;
        }
        return UINT32_MAX;
    }

    if (mock_only_self_present) {
        uint8_t self_pk[32];
        test_keypair_from_id(SELF_SEED_ID, self_pk, NULL, NULL);

        if (memcmp(public_key, self_pk, 32) == 0) {
            if (error != NULL) {
                *error = TOX_ERR_GROUP_PEER_QUERY_OK;
            }
            return 1;
        }

        if (error != NULL) {
            *error = (Tox_Err_Group_Peer_Query)9999;
        }
        return UINT32_MAX;
    }

    if (error != NULL) {
        *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    }
    return 1;
}

bool tox_group_send_custom_packet(const Tox *tox,
                                  uint32_t group_number,
                                  bool lossless,
                                  const uint8_t *data,
                                  size_t length,
                                  Tox_Err_Group_Send_Custom_Packet *error)
{
    (void)tox;
    (void)group_number;
    (void)lossless;

    if (error != NULL) {
        *error = TOX_ERR_GROUP_SEND_CUSTOM_PACKET_OK;
    }

    if (data == NULL || length == 0 || length > 1200) {
        return false;
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
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v & 0xFF);
}

static bool make_presence_packet(uint8_t *out,
                                 size_t cap,
                                 size_t *out_len,
                                 uint32_t signer_id,
                                 uint8_t status,
                                 uint64_t timestamp,
                                 const uint8_t *nick,
                                 uint16_t nick_len)
{
    if (out == NULL || out_len == NULL) {
        return false;
    }
    if (nick_len > MID_MAX_NICK_SIZE) {
        return false;
    }
    if (nick_len > 0 && nick == NULL) {
        return false;
    }

    uint8_t id_pk[32];
    uint8_t sig_pk[32];
    uint8_t sig_sk[64];
    test_keypair_from_id(signer_id, id_pk, sig_pk, sig_sk);

    size_t body_len = 1 + 8 + 2 + nick_len + 32 + 32;
    size_t packet_len = 5 + 64 + body_len;

    if (cap < packet_len || body_len > TEST_BODY_MAX) {
        return false;
    }

    uint8_t body[TEST_BODY_MAX];
    size_t o = 0;

    body[o++] = status;
    put_u64_be(body + o, timestamp);
    o += 8;
    put_u16_be(body + o, nick_len);
    o += 2;

    if (nick_len > 0) {
        memcpy(body + o, nick, nick_len);
        o += nick_len;
    }

    memcpy(body + o, id_pk, 32);  // Identity key (Curve25519)
    o += 32;
    memcpy(body + o, sig_pk, 32); // Signing key (Ed25519)
    o += 32;

    uint8_t sig[64];
    unsigned long long sig_len = 0;

    if (crypto_sign_detached(sig, &sig_len, body, body_len, sig_sk) != 0) {
        return false;
    }
    if (sig_len != 64) {
        return false;
    }

    size_t p = 0;
    out[p++] = MID_MAGIC_0;
    out[p++] = MID_MAGIC_1;
    out[p++] = MID_MAGIC_2;
    out[p++] = MID_PROTOCOL_VERSION;
    out[p++] = MID_MSG_PRESENCE;

    memcpy(out + p, sig, 64);
    p += 64;

    memcpy(out + p, body, body_len);
    p += body_len;

    *out_len = p;
    return true;
}

static bool get_peer_info_by_key(MidState *s,
                                 const uint8_t *chat_id,
                                 const uint8_t key[32],
                                 MidPeerInfo *out)
{
    int idx = mid_find_peer(s, chat_id, key);
    if (idx < 0) {
        return false;
    }
    return mid_peer_list_get(s, chat_id, (size_t)idx, out);
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
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

    /* These must simply not crash. */
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
        if (info.nickname[i] != 'N') {
            all_match = false;
            break;
        }
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

    /* Duplicate join must not duplicate the record. */
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       1000,
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

    /* Older signed timestamp must not replace newer nickname. */
    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       500,
                                       (const uint8_t *)"old", 3),
                  "build old presence packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, peer_pk, &info), "find peer after old packet");
    T_ASSERT_INT_EQ(info.nickname_len, 5, "old packet did not replace nickname length");
    T_ASSERT_TRUE(memcmp(info.nickname, "alice", 5) == 0, "old packet did not replace nickname");

    /* Equal signed timestamp must not replace either. */
    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       1000,
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       1000,
                                       (const uint8_t *)"bob", 3),
                  "build ACTIVE packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);

    T_ASSERT_FALSE(mid_peer_is_signed_left(s, cid1, peer_pk), "not LEFT yet");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_LEFT,
                                       2000,
                                       NULL, 0),
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       5000,
                                       (const uint8_t *)"live", 4),
                  "build ACTIVE packet");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_LEFT,
                                       4000,
                                       NULL, 0),
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_LEFT,
                                       1000,
                                       NULL, 0),
                  "build LEFT packet for unknown peer");
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);

    T_ASSERT_TRUE(mid_peer_is_signed_left(s, cid1, peer_pk), "unknown LEFT tombstone stored");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 2, "tombstone inserted");
    T_ASSERT_INT_EQ(mid_online_count(s, cid1), 1, "tombstone offline");

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       2000,
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       peer_id,
                                       MID_STATUS_ACTIVE,
                                       1000,
                                       (const uint8_t *)"x", 1),
                  "build valid packet");

    /* Corrupt one byte inside the signature. */
    pkt[10] ^= 0xFF;
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, pkt, len);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "corrupt signature rejected");

    uint8_t bad_magic[5] = {0x00, 0x00, 0x00, 0x00, MID_MSG_PRESENCE};
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, bad_magic, sizeof(bad_magic));

    uint8_t short_pkt[4] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION};
    mid_on_group_custom_packet(s, tox, GROUP_1, peer_id, short_pkt, sizeof(short_pkt));

    uint8_t unknown_type[5] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION, 0x7F};
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

    T_ASSERT_TRUE(make_presence_packet(pkt, sizeof(pkt), &len,
                                       SELF_SEED_ID,
                                       MID_STATUS_ACTIVE,
                                       9999,
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

    bool we_were_kicked =
        mid_on_group_moderation(s, tox, GROUP_1, 9, 0, TOX_GROUP_MOD_EVENT_KICK);

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

    bool we_were_kicked =
        mid_on_group_moderation(s, tox, GROUP_1, 0, peer_id, TOX_GROUP_MOD_EVENT_KICK);

    T_ASSERT_FALSE(we_were_kicked, "other peer kick detected");
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group remains");
    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 1, "kicked peer deleted");

    uint8_t peer_pk[32];
    test_keypair_from_id(peer_id, peer_pk, NULL, NULL);
    T_ASSERT_INT_EQ(mid_find_peer(s, cid1, peer_pk), -1, "kicked peer not found");

    mid_free(s);
    return true;
}

static bool test_roster_request_cooldown(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t req[5] = {
        MID_MAGIC_0,
        MID_MAGIC_1,
        MID_MAGIC_2,
        MID_PROTOCOL_VERSION,
        MID_MSG_ROSTER_REQUEST
    };

    /*
     * Roster replies use multicast suppression: a request only schedules a
     * delayed reply (1..5 s). Nothing is sent synchronously.
     */
    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, sizeof(req));
    T_ASSERT_INT_EQ(mock_send_count, 0, "roster request does not reply immediately");

    /*
     * Wait past the maximum randomized delay, then run the iterate loop so
     * the scheduled roster reply is flushed out.
     */
    sleep(6);
    mid_iterate(s, tox);
    T_ASSERT_TRUE(mock_send_count > 0, "scheduled roster reply is eventually sent");

    /*
     * A second request immediately after must again be delayed, not sent
     * synchronously.
     */
    mock_send_count = 0;
    mid_on_group_custom_packet(s, tox, GROUP_1, 1, req, sizeof(req));
    T_ASSERT_INT_EQ(mock_send_count, 0, "second roster request is also delayed");

    mid_free(s);
    return true;
}

static void counting_cb(const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], void *user_data)
{
    (void)chat_id;
    if (user_data != NULL) {
        (*(int *)user_data)++;
    }
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

int main(void)
{
    if (sodium_init() < 0) {
        return 1;
    }

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

    SUITE_END();

    return test_summary("midroster_unit");
}

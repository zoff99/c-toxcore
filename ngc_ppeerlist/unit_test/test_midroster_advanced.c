#include "test_framework.h"
#include "../../toxcore/tox.h"
#include "../mid_roster.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

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
#define GROUP_1                1

static const uint8_t cid1[TOX_GROUP_CHAT_ID_SIZE] = {1};

/* ------------------------------------------------------------------ */
/* Mock state                                                         */
/* ------------------------------------------------------------------ */

static int  mock_send_count        = 0;
static bool mock_capture_packets   = false;
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

static void get_pk(uint32_t id, uint8_t out[32]) {
    test_keypair_from_id(id, out, NULL, NULL);
}


/* ------------------------------------------------------------------ */
/* force_print: bypass test framework stdout capture on PASS          */
/* ------------------------------------------------------------------ */

static void force_print(const char *fmt, ...) {
    FILE *f = fopen("/dev/tty", "w");
    if (!f) f = stderr; /* fallback if no tty available */
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    if (f != stderr) fclose(f);
    else fflush(f);
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
    return 4;
}

bool tox_group_peer_get_name(const Tox *tox, uint32_t group_number, uint32_t peer_id,
                             uint8_t *name, Tox_Err_Group_Peer_Query *error)
{
    (void)tox; (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (!name) return false;
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
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return 1;
}

bool tox_group_send_custom_packet(const Tox *tox, uint32_t group_number, bool lossless,
                                  const uint8_t *data, size_t length,
                                  Tox_Err_Group_Send_Custom_Packet *error)
{
    (void)tox; (void)group_number; (void)lossless;
    if (error) *error = TOX_ERR_GROUP_SEND_CUSTOM_PACKET_OK;
    if (data == NULL || length == 0 || length > 1200) return false;

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

/* Builds the inner record payload (sig + body + eph + cert + nick) */
static size_t build_presence_record(uint8_t *out, size_t cap,
                                    uint32_t signer_id, uint8_t status, uint64_t timestamp,
                                    const uint8_t *nick, uint16_t nick_len)
{
    if (nick_len > MID_MAX_NICK_SIZE) return 0;
    size_t need = 64 + 73 + 32 + 64 + 2 + nick_len;
    if (cap < need) return 0;

    uint8_t id_pk[32], sig_pk[32], sig_sk[64];
    test_keypair_from_id(signer_id, id_pk, sig_pk, sig_sk);

    uint8_t eph_pk[32], eph_sk[64];
    crypto_sign_keypair(eph_pk, eph_sk);

    uint8_t eph_cert[64];
    unsigned long long cert_len = 0;
    crypto_sign_detached(eph_cert, &cert_len, eph_pk, 32, sig_sk);

    uint8_t body[73];
    size_t o = 0;
    body[o++] = status;
    put_u64_be(body + o, timestamp); o += 8;
    memcpy(body + o, id_pk, 32);  o += 32;
    memcpy(body + o, sig_pk, 32); o += 32;

    uint8_t sig[64];
    unsigned long long sig_len = 0;
    crypto_sign_detached(sig, &sig_len, body, 73, eph_sk);

    size_t p = 0;
    memcpy(out + p, sig, 64); p += 64;
    memcpy(out + p, body, 73); p += 73;
    memcpy(out + p, eph_pk, 32); p += 32;
    memcpy(out + p, eph_cert, 64); p += 64;
    put_u16_be(out + p, nick_len); p += 2;
    if (nick_len > 0) {
        memcpy(out + p, nick, nick_len);
        p += nick_len;
    }
    return p;
}

static bool make_presence_packet(uint8_t *out, size_t cap, size_t *out_len,
                                 uint32_t signer_id, uint8_t status, uint64_t timestamp,
                                 const uint8_t *nick, uint16_t nick_len)
{
    size_t need = 5 + 64 + 73 + 32 + 64 + 2 + nick_len + 1;
    if (cap < need) return false;

    size_t p = 0;
    out[p++] = MID_MAGIC_0;
    out[p++] = MID_MAGIC_1;
    out[p++] = MID_MAGIC_2;
    out[p++] = MID_PROTOCOL_VERSION;
    out[p++] = MID_MSG_PRESENCE;

    size_t rec_len = build_presence_record(out + p, cap - p, signer_id, status, timestamp, nick, nick_len);
    if (rec_len == 0) return false;
    p += rec_len;

    out[p++] = 0; /* pad_len */
    *out_len = p;
    return true;
}

static bool make_heartbeat_packet(uint8_t *out, size_t cap, size_t *out_len,
                                  uint32_t signer_id, uint64_t timestamp)
{
    size_t need = 5 + 64 + 137 + 1;
    if (cap < need) return false;

    uint8_t id_pk[32], sig_pk[32], sig_sk[64];
    test_keypair_from_id(signer_id, id_pk, sig_pk, sig_sk);

    uint8_t eph_pk[32], eph_sk[64];
    crypto_sign_keypair(eph_pk, eph_sk);

    uint8_t eph_cert[64];
    unsigned long long cert_len = 0;
    crypto_sign_detached(eph_cert, &cert_len, eph_pk, 32, sig_sk);

    uint8_t body[137];
    size_t o = 0;
    body[o++] = MID_STATUS_ACTIVE;
    put_u64_be(body + o, timestamp); o += 8;
    memcpy(body + o, id_pk, 32); o += 32;
    memcpy(body + o, eph_pk, 32); o += 32;
    memcpy(body + o, eph_cert, 64); o += 64;

    uint8_t sig[64];
    unsigned long long sig_len = 0;
    crypto_sign_detached(sig, &sig_len, body, 137, eph_sk);

    size_t p = 0;
    out[p++] = MID_MAGIC_0;
    out[p++] = MID_MAGIC_1;
    out[p++] = MID_MAGIC_2;
    out[p++] = MID_PROTOCOL_VERSION;
    out[p++] = MID_MSG_HEARTBEAT;

    memcpy(out + p, sig, 64); p += 64;
    memcpy(out + p, body, 137); p += 137;

    out[p++] = 0; /* pad_len */
    *out_len = p;
    return true;
}

static bool make_roster_batch_packet(uint8_t *out, size_t cap, size_t *out_len,
                                     const uint8_t *fp,
                                     uint32_t peer_id1, uint64_t ts1, const uint8_t *nick1, uint16_t nick_len1,
                                     uint32_t peer_id2, uint64_t ts2, const uint8_t *nick2, uint16_t nick_len2)
{
    uint8_t rec1[512], rec2[512];
    size_t len1 = build_presence_record(rec1, sizeof(rec1), peer_id1, MID_STATUS_ACTIVE, ts1, nick1, nick_len1);
    size_t len2 = build_presence_record(rec2, sizeof(rec2), peer_id2, MID_STATUS_ACTIVE, ts2, nick2, nick_len2);
    if (len1 == 0 || len2 == 0) return false;

    size_t need = 5 + 32 + 2 + len1 + len2 + 1;
    if (cap < need) return false;

    size_t p = 0;
    out[p++] = MID_MAGIC_0;
    out[p++] = MID_MAGIC_1;
    out[p++] = MID_MAGIC_2;
    out[p++] = MID_PROTOCOL_VERSION;
    out[p++] = MID_MSG_ROSTER_BATCH;

    memcpy(out + p, fp, 32); p += 32;
    put_u16_be(out + p, 2); p += 2; /* count = 2 */

    memcpy(out + p, rec1, len1); p += len1;
    memcpy(out + p, rec2, len2); p += len2;

    out[p++] = 0; /* pad_len */
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

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static bool test_save_and_load_unencrypted(void)
{
    const char *path = "/tmp/mid_test_unencrypted.dat";
    unlink(path);

    MidState *s1 = mid_new(path, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s1, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512];
    size_t len = 0;
    uint32_t peer1 = 3001;
    uint32_t peer2 = 3002;

    make_presence_packet(pkt, sizeof(pkt), &len, peer1, MID_STATUS_ACTIVE, 1000, (const uint8_t *)"alice", 5);
    mid_on_group_custom_packet(s1, tox, GROUP_1, peer1, pkt, len);

    make_presence_packet(pkt, sizeof(pkt), &len, peer2, MID_STATUS_LEFT, 2000, NULL, 0);
    mid_on_group_custom_packet(s1, tox, GROUP_1, peer2, pkt, len);

    T_ASSERT_INT_EQ(mid_peer_count(s1, cid1), 3, "3 peers before save");

    uint8_t pk2[32]; get_pk(peer2, pk2);
    T_ASSERT_TRUE(mid_peer_is_signed_left(s1, cid1, pk2), "peer2 is LEFT");

    T_ASSERT_TRUE(mid_save(s1, NULL, 0), "save unencrypted");
    mid_free(s1);

    MidState *s2 = mid_new(path, NULL, 0);
    T_ASSERT_INT_EQ(mid_group_count(s2), 1, "group loaded");
    T_ASSERT_INT_EQ(mid_peer_count(s2, cid1), 3, "3 peers loaded");

    uint8_t pk1[32]; get_pk(peer1, pk1);
    MidPeerInfo info1, info2;
    T_ASSERT_TRUE(get_peer_info_by_key(s2, cid1, pk1, &info1), "find peer1");
    T_ASSERT_INT_EQ(info1.nickname_len, 5, "peer1 nick len");
    T_ASSERT_TRUE(memcmp(info1.nickname, "alice", 5) == 0, "peer1 nick");

    T_ASSERT_TRUE(get_peer_info_by_key(s2, cid1, pk2, &info2), "find peer2");
    T_ASSERT_INT_EQ(info2.status, MID_STATUS_LEFT, "peer2 status LEFT");

    mid_free(s2);
    unlink(path);
    return true;
}

static bool test_save_and_load_encrypted(void)
{
    const char *path = "/tmp/mid_test_encrypted.dat";
    unlink(path);
    const uint8_t pass[] = "correct horse battery staple";
    size_t pass_len = strlen((const char*)pass);

    MidState *s1 = mid_new(path, pass, pass_len);
    Tox *tox = create_dummy_tox();
    mid_on_group_self_join(s1, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t pkt[512]; size_t len = 0;
    uint32_t peer1 = 4001;
    make_presence_packet(pkt, sizeof(pkt), &len, peer1, MID_STATUS_ACTIVE, 1000, (const uint8_t *)"bob", 3);
    mid_on_group_custom_packet(s1, tox, GROUP_1, peer1, pkt, len);

    T_ASSERT_TRUE(mid_save(s1, pass, pass_len), "save encrypted");
    mid_free(s1);

    MidState *s2 = mid_new(path, pass, pass_len);
    T_ASSERT_INT_EQ(mid_peer_count(s2, cid1), 2, "peers loaded with correct pass");
    mid_free(s2);

    const uint8_t wrong_pass[] = "wrong password";
    MidState *s3 = mid_new(path, wrong_pass, strlen((const char*)wrong_pass));
    T_ASSERT_INT_EQ(mid_peer_count(s3, cid1), 0, "no peers loaded with wrong pass");
    mid_free(s3);

    unlink(path);
    return true;
}

static bool test_roster_batch_fingerprint_suppression(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint8_t req[6] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION, MID_MSG_ROSTER_REQUEST, 0};
    mid_on_group_custom_packet(s, tox, GROUP_1, 9999, req, 6);

    uint32_t peer1 = 5001;
    uint8_t pkt[512]; size_t len = 0;
    make_presence_packet(pkt, sizeof(pkt), &len, peer1, MID_STATUS_ACTIVE, 1000, (const uint8_t *)"p1", 2);
    mid_on_group_custom_packet(s, tox, GROUP_1, peer1, pkt, len);

    uint8_t self_pk[32], peer1_pk[32];
    get_pk(SELF_SEED_ID, self_pk);
    get_pk(peer1, peer1_pk);

    uint8_t fp[32];
    for(int i=0; i<32; i++) fp[i] = self_pk[i] ^ peer1_pk[i];

    uint8_t batch[1024]; size_t batch_len = 0;
    make_roster_batch_packet(batch, sizeof(batch), &batch_len, fp,
                             SELF_SEED_ID, 1000, (const uint8_t *)"self", 4,
                             peer1, 1000, (const uint8_t *)"p1", 2);

    int sends_before = mock_send_count;
    mid_on_group_custom_packet(s, tox, GROUP_1, 8888, batch, batch_len);

    sleep(6);
    mid_iterate(s, tox);

    T_ASSERT_INT_EQ(mock_send_count, sends_before, "roster reply suppressed by matching batch");

    mid_free(s);
    return true;
}

static bool test_inbound_roster_batch_processing(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint32_t p1 = 6001, p2 = 6002;
    uint8_t p1_pk[32], p2_pk[32];
    get_pk(p1, p1_pk); get_pk(p2, p2_pk);

    uint8_t fp[32];
    for(int i=0; i<32; i++) fp[i] = p1_pk[i] ^ p2_pk[i];

    uint8_t batch[1024]; size_t batch_len = 0;
    make_roster_batch_packet(batch, sizeof(batch), &batch_len, fp,
                             p1, 1000, (const uint8_t *)"alice", 5,
                             p2, 2000, (const uint8_t *)"bob", 3);

    mid_on_group_custom_packet(s, tox, GROUP_1, 7777, batch, batch_len);

    T_ASSERT_INT_EQ(mid_peer_count(s, cid1), 3, "self + 2 peers from batch");
    
    MidPeerInfo info1, info2;
    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, p1_pk, &info1), "find p1");
    T_ASSERT_INT_EQ(info1.nickname_len, 5, "p1 nick");
    T_ASSERT_TRUE(info1.connection_status == TOX_CONNECTION_NONE, "p1 offline from batch");

    T_ASSERT_TRUE(get_peer_info_by_key(s, cid1, p2_pk, &info2), "find p2");
    T_ASSERT_INT_EQ(info2.nickname_len, 3, "p2 nick");

    mid_free(s);
    return true;
}

static bool test_inbound_heartbeat_updates_last_seen(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    uint32_t peer1 = 7001;
    uint8_t pkt[512]; size_t len = 0;
    
    make_presence_packet(pkt, sizeof(pkt), &len, peer1, MID_STATUS_ACTIVE, 1000, (const uint8_t *)"hb_test", 7);
    mid_on_group_custom_packet(s, tox, GROUP_1, peer1, pkt, len);

    uint8_t peer1_pk[32];
    get_pk(peer1, peer1_pk);

    MidPeerInfo info_before;
    get_peer_info_by_key(s, cid1, peer1_pk, &info_before);
    uint64_t last_seen_before = info_before.last_seen;

    sleep(2);

    uint8_t hb[256]; size_t hb_len = 0;
    make_heartbeat_packet(hb, sizeof(hb), &hb_len, peer1, 2000);
    mid_on_group_custom_packet(s, tox, GROUP_1, peer1, hb, hb_len);

    MidPeerInfo info_after;
    get_peer_info_by_key(s, cid1, peer1_pk, &info_after);

    T_ASSERT_TRUE(info_after.last_seen > last_seen_before, "last_seen updated by heartbeat");
    T_ASSERT_INT_EQ(info_after.nickname_len, 7, "nickname unchanged by heartbeat");
    T_ASSERT_TRUE(memcmp(info_after.nickname, "hb_test", 7) == 0, "nickname bytes unchanged");

    mid_free(s);
    return true;
}

static bool test_self_set_name_does_not_broadcast(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"old_name", 8);
    
    mock_send_count = 0;

    bool changed = mid_self_set_name(s, tox, GROUP_1, (const uint8_t *)"new_name", 8);
    T_ASSERT_TRUE(changed, "name change reported as changed");
    T_ASSERT_INT_EQ(mock_send_count, 0, "no custom packet broadcast on name change");

    uint8_t self_pk[32];
    get_pk(SELF_SEED_ID, self_pk);
    MidPeerInfo info;
    get_peer_info_by_key(s, cid1, self_pk, &info);
    T_ASSERT_INT_EQ(info.nickname_len, 8, "local nick len updated");
    T_ASSERT_TRUE(memcmp(info.nickname, "new_name", 8) == 0, "local nick bytes updated");

    mid_free(s);
    return true;
}

/* ------------------------------------------------------------------ */
/* Network-stats load test: 100 peers joining, 50 leaving            */
/* ------------------------------------------------------------------ */

#define LOAD_TEST_PEER_COUNT   100
#define LOAD_TEST_LEAVE_COUNT   50

static bool test_network_stats_large_group_simulation(void)
{
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    /* Per-peer counters: indexed 1..LOAD_TEST_PEER_COUNT */
    uint64_t peer_pkt_count[LOAD_TEST_PEER_COUNT + 1];
    uint64_t peer_bytes_in[LOAD_TEST_PEER_COUNT + 1];
    memset(peer_pkt_count, 0, sizeof(peer_pkt_count));
    memset(peer_bytes_in,  0, sizeof(peer_bytes_in));

    mock_send_count = 0;

    /* 1. Self joins */
    mid_on_group_self_join(s, tox, GROUP_1, (const uint8_t *)"self", 4);

    /* 2. Simulate 100 peers joining and sending their signed PRESENCE */
    for (uint32_t i = 1; i <= LOAD_TEST_PEER_COUNT; i++) {
        mid_on_group_peer_join(s, tox, GROUP_1, i);

        uint8_t pkt[512];
        size_t  len = 0;
        char    nick[16];
        int     nick_len = snprintf(nick, sizeof(nick), "p%u", (unsigned)i);

        make_presence_packet(pkt, sizeof(pkt), &len, i, MID_STATUS_ACTIVE,
                             1000 + i,
                             (const uint8_t *)nick, (uint16_t)nick_len);
        mid_on_group_custom_packet(s, tox, GROUP_1, i, pkt, len);

        peer_pkt_count[i]++;
        peer_bytes_in[i] += len;
    }

    /* 3. Simulate 50 peers leaving (signed LEFT tombstones) */
    for (uint32_t i = 1; i <= LOAD_TEST_LEAVE_COUNT; i++) {
        uint8_t pkt[512];
        size_t  len = 0;

        make_presence_packet(pkt, sizeof(pkt), &len, i, MID_STATUS_LEFT,
                             2000 + i, NULL, 0);
        mid_on_group_custom_packet(s, tox, GROUP_1, i, pkt, len);

        peer_pkt_count[i]++;
        peer_bytes_in[i] += len;
    }

    /* 4. Force iterate past the MID_SYNC_INTERVAL_SEC throttle to flush
     *    any pending ROSTER_BATCH reply. */
    sleep(6);
    mid_iterate(s, tox);

    /* 5. Collect totals from the middleware */
    uint64_t total_sent = 0, total_recv = 0;
    mid_get_network_stats(s, &total_sent, &total_recv);

    /* 6. Compute our own per-peer sums to cross-check */
    uint64_t sum_recv_check = 0;
    uint64_t sum_pkt_count  = 0;
    for (uint32_t i = 1; i <= LOAD_TEST_PEER_COUNT; i++) {
        sum_recv_check += peer_bytes_in[i];
        sum_pkt_count  += peer_pkt_count[i];
    }

    /* -------------------------------------------------------------- */
    /* Stats report via force_print (bypasses framework capture)      */
    /* -------------------------------------------------------------- */
    force_print("\n");
    force_print("      ============================================================\n");
    force_print("        NETWORK STATS: %d peers, %d leaves simulation\n",
                LOAD_TEST_PEER_COUNT, LOAD_TEST_LEAVE_COUNT);
    force_print("      ------------------------------------------------------------\n");
    force_print("        Per-peer received bytes:\n");

    for (uint32_t i = 1; i <= LOAD_TEST_PEER_COUNT; i++) {
        const char *pkt_word = (peer_pkt_count[i] == 1) ? "pkt" : "pkts";
        force_print("          Peer %3u:  %6llu bytes  (%2llu %s)\n",
                    (unsigned)i,
                    (unsigned long long)peer_bytes_in[i],
                    (unsigned long long)peer_pkt_count[i],
                    pkt_word);
    }

    force_print("      ------------------------------------------------------------\n");
    force_print("        Sum of per-peer received:     %8llu bytes  (%3llu pkts)\n",
                (unsigned long long)sum_recv_check,
                (unsigned long long)sum_pkt_count);
    force_print("        Middleware recv_bytes total:  %8llu bytes\n",
                (unsigned long long)total_recv);
    force_print("        Middleware sent_bytes total:  %8llu bytes\n",
                (unsigned long long)total_sent);
    force_print("        Packets sent (mock counter):  %8d pkts\n",
                mock_send_count);
    force_print("      ------------------------------------------------------------\n");
    force_print("        Avg bytes sent per peer:      %8.2f bytes\n",
                (double)total_sent / (double)LOAD_TEST_PEER_COUNT);
    force_print("        Avg bytes received per peer:  %8.2f bytes\n",
                (double)total_recv / (double)LOAD_TEST_PEER_COUNT);
    force_print("      ============================================================\n\n");

    /* -------------------------------------------------------------- */
    /* Sanity assertions                                              */
    /* -------------------------------------------------------------- */

    /* Per-peer sum must match middleware recv_bytes exactly. */
    T_ASSERT_TRUE(sum_recv_check == total_recv,
                  "per-peer recv sum matches middleware recv_bytes");

    /* 100 joins + 50 leaves = 150 packets received. */
    T_ASSERT_TRUE(sum_pkt_count ==
                  LOAD_TEST_PEER_COUNT + LOAD_TEST_LEAVE_COUNT,
                  "150 packets received total");

    /* Per-peer bytes must be > 0 for every peer. */
    bool all_nonzero = true;
    for (uint32_t i = 1; i <= LOAD_TEST_PEER_COUNT; i++) {
        if (peer_bytes_in[i] == 0) { all_nonzero = false; break; }
    }
    T_ASSERT_TRUE(all_nonzero, "every peer contributed bytes");

    /* Peers 1..50 sent 2 packets (join + leave), peers 51..100 sent 1. */
    for (uint32_t i = 1; i <= LOAD_TEST_LEAVE_COUNT; i++) {
        T_ASSERT_TRUE(peer_pkt_count[i] == 2, "leaving peer sent 2 pkts");
    }
    for (uint32_t i = LOAD_TEST_LEAVE_COUNT + 1; i <= LOAD_TEST_PEER_COUNT; i++) {
        T_ASSERT_TRUE(peer_pkt_count[i] == 1, "non-leaving peer sent 1 pkt");
    }

    /*
     * Bounds on sent bytes.
     *
     * Sent traffic is dominated by:
     *   - 1 self PRESENCE + 1 ROSTER_REQUEST at join
     *   - ~21 ROSTER_BATCH packets (101 signed records, ~5 per batch)
     *   - each packet padded with up to 64 random bytes
     *
     * Typical total is 30-60 KB; 20-70 KB bounds give wide slack for
     * padding randomness while still catching a massive overhead
     * regression (e.g. losing the batch compressor).
     */
    T_ASSERT_TRUE(total_sent > 20000,  "sent > 20 KB");
    T_ASSERT_TRUE(total_sent < 70000,  "sent < 70 KB (no massive overhead)");

    /*
     * Bounds on received bytes.
     * 150 V4 presence packets × ~200 bytes each (with padding) ≈ 30 KB.
     */
    T_ASSERT_TRUE(total_recv > 25000,  "recv > 25 KB");
    T_ASSERT_TRUE(total_recv < 50000,  "recv < 50 KB (no massive overhead)");

    mid_free(s);
    return true;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    if (sodium_init() < 0) return 1;

    TEST_SUITE("midroster advanced / persistence & wire tests");

    RUN_TEST(test_save_and_load_unencrypted);
    RUN_TEST(test_save_and_load_encrypted);
    RUN_TEST(test_roster_batch_fingerprint_suppression);
    RUN_TEST(test_inbound_roster_batch_processing);
    RUN_TEST(test_inbound_heartbeat_updates_last_seen);
    RUN_TEST(test_self_set_name_does_not_broadcast);
    RUN_TEST(test_network_stats_large_group_simulation);

    SUITE_END();
    return test_summary("midroster_advanced");
}

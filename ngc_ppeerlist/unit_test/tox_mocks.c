#include "../../toxcore/tox.h"
#include "../../toxencryptsave/toxencryptsave.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <sodium.h>

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

/* When set to 1, each peer_id gets a DISTINCT identity key so the
 * middleware roster actually grows/shrinks under the threading test. */
static volatile int g_mock_churn = 1;
void mock_set_churn(int on) { g_mock_churn = on; }

/* ── deterministic valid Ed25519 keypair for "self" ───────────── */

/* ── deterministic valid keypair for "self" ───────────── */
static void mock_self_keys(uint8_t id_pk[32], uint8_t sig_pk[32], uint8_t sig_sk[64])
{
    uint8_t seed[32];
    memset(seed, 0, 32);
    seed[0] = 0x5A;
    seed[1] = 0x01;
    seed[2] = 0x02;
    seed[3] = 0x03;
    seed[4] = 0x04;
    seed[5] = 0x43;
    
    /* 1. Generate Ed25519 signing keypair */
    crypto_sign_seed_keypair(sig_pk, sig_sk, seed);
    
    /* 2. Derive Curve25519 identity key from Ed25519 signing key */
    if (crypto_sign_ed25519_pk_to_curve25519(id_pk, sig_pk) != 0) {
        memcpy(id_pk, sig_pk, 32); /* Fallback, should never happen with valid keys */
    }
}

/* ── deterministic valid keypair for churned peers ────── */
static void mock_churn_peer_keys(uint32_t peer_id, uint8_t id_pk[32], uint8_t sig_pk[32])
{
    uint8_t seed[32];
    uint8_t sk[64];
    
    memset(seed, 0, 32);
    seed[0] = (uint8_t)(peer_id >> 24);
    seed[1] = (uint8_t)(peer_id >> 16);
    seed[2] = (uint8_t)(peer_id >> 8);
    seed[3] = (uint8_t)(peer_id);
    
    /* 1. Generate Ed25519 signing keypair */
    crypto_sign_seed_keypair(sig_pk, sk, seed);
    
    /* 2. Derive Curve25519 identity key from Ed25519 signing key */
    if (crypto_sign_ed25519_pk_to_curve25519(id_pk, sig_pk) != 0) {
        memcpy(id_pk, sig_pk, 32); /* Fallback, should never happen with valid keys */
    }
}

/* ── churn identity (unchanged) ─────────────────────────────────── */

static inline uint64_t churn_splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline void churn_put_le64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}


/******************************************************************************
Group Chat ID / Group Number mocks
FIX: encode group_number into chat_id so each group_number gets a unique chat_id.
******************************************************************************/

__attribute__((weak))
uint32_t tox_group_by_chat_id(const Tox *tox, const uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (chat_id == NULL) return UINT32_MAX;
    return (uint32_t)chat_id[0] |
           ((uint32_t)chat_id[1] << 8) |
           ((uint32_t)chat_id[2] << 16) |
           ((uint32_t)chat_id[3] << 24);
}

__attribute__((weak))
bool tox_group_get_chat_id(const Tox *tox, uint32_t group_number, uint8_t *chat_id, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (chat_id != NULL) {
        memset(chat_id, 0, TOX_GROUP_CHAT_ID_SIZE);
        chat_id[0] = (uint8_t)(group_number & 0xFF);
        chat_id[1] = (uint8_t)((group_number >> 8) & 0xFF);
        chat_id[2] = (uint8_t)((group_number >> 16) & 0xFF);
        chat_id[3] = (uint8_t)((group_number >> 24) & 0xFF);
    }
    return true;
}


/******************************************************************************
Self Key mocks
******************************************************************************/

__attribute__((weak))
bool tox_group_self_get_public_key(const Tox *tox, uint32_t group_number, uint8_t *public_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (public_key == NULL) return false;
    
    uint8_t sig_pk[32], sig_sk[64];
    mock_self_keys(public_key, sig_pk, sig_sk); // Returns Curve25519 identity key
    return true;
}

__attribute__((weak))
bool tox_group_self_get_signing_public_key(const Tox *tox, uint32_t group_number, uint8_t *public_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (public_key == NULL) return false;
    
    uint8_t id_pk[32], sig_sk[64];
    mock_self_keys(id_pk, public_key, sig_sk); // Returns Ed25519 signing key
    return true;
}

__attribute__((weak))
bool tox_group_self_get_signing_secret_key(const Tox *tox, uint32_t group_number, uint8_t *secret_key, Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    if (secret_key == NULL) return false;
    
    uint8_t id_pk[32], sig_pk[32];
    mock_self_keys(id_pk, sig_pk, secret_key); // Returns Ed25519 secret key
    return true;
}

/******************************************************************************
Peer Key mocks
******************************************************************************/

__attribute__((weak))
bool tox_group_peer_get_public_key(const Tox *tox, uint32_t group_number, uint32_t peer_id, uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (public_key == NULL) return false;
    
    if (g_mock_churn) {
        uint8_t sig_pk[32];
        mock_churn_peer_keys(peer_id, public_key, sig_pk); // Returns Curve25519 identity key
    } else {
        memset(public_key, 0xCC, 32);
    }
    return true;
}

__attribute__((weak))
bool tox_group_peer_get_signing_public_key(const Tox *tox, uint32_t group_number, uint32_t peer_id, uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (public_key == NULL) return false;
    
    if (g_mock_churn) {
        uint8_t id_pk[32];
        mock_churn_peer_keys(peer_id, id_pk, public_key); // Returns Ed25519 signing key
    } else {
        memset(public_key, 0xCC, 32);
    }
    return true;
}

__attribute__((weak))
uint32_t tox_group_self_get_peer_id(const Tox *tox, uint32_t group_number, Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    return 0;
}

__attribute__((weak))
size_t tox_group_peer_get_name_size(const Tox *tox, uint32_t group_number, uint32_t peer_id, Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return 4;
}

__attribute__((weak))
bool tox_group_peer_get_name(const Tox *tox, uint32_t group_number, uint32_t peer_id, uint8_t *name, Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    if (name == NULL) return false;
    name[0] = 'n';
    name[1] = (uint8_t)((peer_id >> 8) & 0xFF);
    name[2] = (uint8_t)(peer_id & 0xFF);
    name[3] = '!';
    return true;
}

__attribute__((weak))
uint32_t tox_group_peer_by_public_key(const Tox *tox, uint32_t group_number, const uint8_t *public_key, Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return 1;
}

__attribute__((weak))
bool tox_group_send_custom_packet(const Tox *tox, uint32_t group_number, bool lossless, const uint8_t *data, size_t length, Tox_Err_Group_Send_Custom_Packet *error)
{
    (void)tox;
    (void)group_number;
    (void)lossless;
    if (error) *error = TOX_ERR_GROUP_SEND_CUSTOM_PACKET_OK;
    return true;
}

__attribute__((weak))
Tox_Connection tox_group_peer_get_connection_status(const Tox *tox, uint32_t group_number, uint32_t peer_id,
        Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return TOX_CONNECTION_NONE;
}

__attribute__((weak))
Tox_Group_Role tox_group_peer_get_role(const Tox *tox, uint32_t group_number, uint32_t peer_id,
                                       Tox_Err_Group_Peer_Query *error)
{
    (void)tox;
    (void)group_number;
    (void)peer_id;
    if (error) *error = TOX_ERR_GROUP_PEER_QUERY_OK;
    return (Tox_Group_Role)1;
}

__attribute__((weak))
Tox_Group_Role tox_group_self_get_role(const Tox *tox, uint32_t group_number, Tox_Err_Group_Self_Query *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_SELF_QUERY_OK;
    return (Tox_Group_Role)0;
}

__attribute__((weak))
bool tox_group_get_founder_public_key(const Tox *tox, uint32_t group_number, uint8_t *public_key, Tox_Err_Group_State_Queries *error)
{
    (void)tox;
    (void)group_number;
    if (error) *error = TOX_ERR_GROUP_STATE_QUERIES_OK;
    if (public_key == NULL) return false;
    
    /* 
     * Use a distinct, deterministic pattern for the founder identity key.
     * 0xF0 is used here to represent "FounDer". 
     */
    memset(public_key, 0xF0, 32);
    return true;
}

/******************************************************************************
 * toxencryptsave mocks
 *
 * Mock encryption uses a simple XOR with the passphrase for testing purposes.
 * The real implementation uses scrypt + XSalsa20-Poly1305.
 *
 * Encrypted format (mock):
 *   [4 bytes magic "TOXE"] [32 bytes salt (zeroed)] [24 bytes nonce (zeroed)]
 *   [16 bytes MAC placeholder] [payload XORed with passphrase]
 *
 * TOX_PASS_ENCRYPTION_EXTRA_LENGTH = 80 = 4 + 32 + 24 + 16 + 4 (padding)
 * We use exactly 80 bytes of overhead to match the real API contract.
 *****************************************************************************/

#define MOCK_TOX_ENC_MAGIC "TOXE"
#define MOCK_TOX_ENC_MAGIC_LEN 4
#define MOCK_TOX_ENC_OVERHEAD 80  /* must match TOX_PASS_ENCRYPTION_EXTRA_LENGTH */

bool tox_is_data_encrypted(const uint8_t *data)
{
    if (data == NULL) return false;
    return memcmp(data, MOCK_TOX_ENC_MAGIC, MOCK_TOX_ENC_MAGIC_LEN) == 0;
}

bool tox_pass_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                      const uint8_t *passphrase, size_t passphrase_len,
                      uint8_t *ciphertext, Tox_Err_Encryption *error)
{
    if (!plaintext || !ciphertext || (!passphrase && passphrase_len > 0)) {
        if (error) *error = TOX_ERR_ENCRYPTION_NULL;
        return false;
    }

    if (plaintext_len == 0) {
        if (error) *error = TOX_ERR_ENCRYPTION_NULL;
        return false;
    }

    /* Write 80-byte header */
    memset(ciphertext, 0, MOCK_TOX_ENC_OVERHEAD);
    memcpy(ciphertext, MOCK_TOX_ENC_MAGIC, MOCK_TOX_ENC_MAGIC_LEN);

    /* XOR payload with passphrase (repeating key) */
    uint8_t *out = ciphertext + MOCK_TOX_ENC_OVERHEAD;
    for (size_t i = 0; i < plaintext_len; i++) {
        if (passphrase && passphrase_len > 0) {
            out[i] = plaintext[i] ^ passphrase[i % passphrase_len];
        } else {
            out[i] = plaintext[i];
        }
    }

    if (error) *error = TOX_ERR_ENCRYPTION_OK;
    return true;
}

bool tox_pass_decrypt(const uint8_t *ciphertext, size_t ciphertext_len,
                      const uint8_t *passphrase, size_t passphrase_len,
                      uint8_t *plaintext, Tox_Err_Decryption *error)
{
    if (!ciphertext || !plaintext) {
        if (error) *error = TOX_ERR_DECRYPTION_NULL;
        return false;
    }

    if (ciphertext_len < MOCK_TOX_ENC_OVERHEAD) {
        if (error) *error = TOX_ERR_DECRYPTION_INVALID_LENGTH;
        return false;
    }

    if (memcmp(ciphertext, MOCK_TOX_ENC_MAGIC, MOCK_TOX_ENC_MAGIC_LEN) != 0) {
        if (error) *error = TOX_ERR_DECRYPTION_BAD_FORMAT;
        return false;
    }

    size_t payload_len = ciphertext_len - MOCK_TOX_ENC_OVERHEAD;
    const uint8_t *in = ciphertext + MOCK_TOX_ENC_OVERHEAD;

    /* Reverse XOR */
    for (size_t i = 0; i < payload_len; i++) {
        if (passphrase && passphrase_len > 0) {
            plaintext[i] = in[i] ^ passphrase[i % passphrase_len];
        } else {
            plaintext[i] = in[i];
        }
    }

    if (error) *error = TOX_ERR_DECRYPTION_OK;
    return true;
}

/******************************************************************************
Dummy Tox Instance
******************************************************************************/

typedef struct DummyTox { int dummy; } DummyTox;

__attribute__((weak))
Tox *create_dummy_tox(void) {
    static DummyTox t;
    return (Tox *)&t;
}

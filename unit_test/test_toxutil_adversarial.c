/*
 * test_toxutil_adversarial.c
 *
 * Adversarial / security tests for toxutil.c, targeting specific bug classes:
 *   - OOB write in all 3 MessageV2 chunk branches (SEND/SYNC/ANSWER)
 *   - Off-by-one at the exact buffer boundary
 *   - Integer / huge-value overflow of `position` and `file_size`
 *   - OOB read in the ANSWER completion path (malformed message)
 *   - Double-completion / use-after-free of a transfer entry
 *   - NULL / invalid inputs
 *   - State corruption via lifecycle events (disconnect, delete)
 *   - Malformed capability packets and protocol edge cases
 *
 * Build/run under sanitizers:  make test_san
 */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <tox.h>
#include <toxutil.h>

/* Internal toxutil callbacks (non-static in amalgamation) */
extern void tox_utils_file_recv_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint32_t kind, uint64_t file_size,
        const uint8_t *filename, size_t filename_length, void *user_data);
extern void tox_utils_file_recv_chunk_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint64_t position, const uint8_t *data,
        size_t length, void *user_data);
extern void tox_utils_file_recv_control_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, TOX_FILE_CONTROL control, void *user_data);
extern void tox_utils_self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data);
extern void tox_utils_friend_connection_status_cb(Tox *tox, uint32_t friendnumber,
        TOX_CONNECTION connection_status, void *user_data);
extern void tox_utils_friend_lossless_packet_cb(Tox *tox, uint32_t friend_number,
        const uint8_t *data, size_t length, void *user_data);
extern bool tox_utils_friend_delete(Tox *tox, uint32_t friend_number, TOX_ERR_FRIEND_DELETE *error);
extern int64_t tox_util_friend_send_message_v2(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type,
                                               uint32_t ts_sec, const uint8_t *message, size_t length,
                                               uint8_t *raw_message_back, uint32_t *raw_msg_len_back,
                                               uint8_t *msgid_back, TOX_ERR_FRIEND_SEND_MESSAGE *error);

#ifndef TOX_MAX_FILETRANSFER_SIZE_MSGV2
#define TOX_MAX_FILETRANSFER_SIZE_MSGV2 (512 * 1024)
#endif
#define BUFSZ TOX_MAX_FILETRANSFER_SIZE_MSGV2

#ifndef TOX_MESSAGEV2_MAX_TEXT_LENGTH
#define TOX_MESSAGEV2_MAX_TEXT_LENGTH (50 * 1024)
#endif

/* ── helper: create Tox + one friend, return friend_number ─────── */
static Tox *setup_with_friend(uint32_t *out_fn)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    Tox *tox = tox_utils_new(&opts, NULL);
    if (!tox) return NULL;
    uint8_t pk[TOX_PUBLIC_KEY_SIZE];
    memset(pk, 0x42, sizeof(pk));
    uint32_t fn = tox_friend_add_norequest(tox, pk, NULL);
    if (out_fn) *out_fn = fn;
    return tox;
}

/* ================================================================
 * OOB WRITE — all three MessageV2 branches
 * Each should be caught by ASan as a heap-buffer-overflow.
 * ================================================================ */
static bool overflow_in_branch(uint32_t kind)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    uint64_t claimed = (uint64_t)BUFSZ + 1000000ULL;
    tox_utils_file_recv_cb(tox, fn, 0, kind, claimed,
                           (const uint8_t *)"x", 1, NULL);

    uint8_t chunk[64];
    memset(chunk, 'A', sizeof(chunk));
    uint64_t evil_pos = (uint64_t)BUFSZ + 1024ULL;
    /* triggers memcpy(data_ + evil_pos, chunk, 64) → OOB write */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, evil_pos, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_oob_write_SEND(void)   { return overflow_in_branch(TOX_FILE_KIND_MESSAGEV2_SEND); }
bool test_oob_write_SYNC(void)   { return overflow_in_branch(TOX_FILE_KIND_MESSAGEV2_SYNC); }
bool test_oob_write_ANSWER(void) { return overflow_in_branch(TOX_FILE_KIND_MESSAGEV2_ANSWER); }

/* ================================================================
 * OFF-BY-ONE BOUNDARY — catches a bad fix
 * ================================================================ */
bool test_boundary_exact_fit_ok(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* position + length == BUFSZ exactly → last valid byte, MUST be safe */
    uint8_t chunk[64];
    memset(chunk, 'B', sizeof(chunk));
    uint64_t pos = (uint64_t)BUFSZ - sizeof(chunk);
    tox_utils_file_recv_chunk_cb(tox, fn, 0, pos, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_boundary_one_past_overflows(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* position + length == BUFSZ + 1 → writes ONE byte past the end */
    uint8_t chunk[64];
    memset(chunk, 'C', sizeof(chunk));
    uint64_t pos = (uint64_t)BUFSZ - sizeof(chunk) + 1;
    tox_utils_file_recv_chunk_cb(tox, fn, 0, pos, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_position_exactly_at_end(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* position == BUFSZ, length > 0 → starts exactly one past the end */
    uint8_t chunk[16];
    memset(chunk, 'D', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, (uint64_t)BUFSZ, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

/* ================================================================
 * INTEGER / HUGE-VALUE overflow
 * ================================================================ */
bool test_huge_position_u64max(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, UINT64_MAX,
                           (const uint8_t *)"x", 1, NULL);

    uint8_t chunk[8];
    memset(chunk, 'E', sizeof(chunk));
    /* position near UINT64_MAX: a naive `position + length` bounds check
     * would overflow and wrap, possibly passing validation. */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, UINT64_MAX - 2, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_huge_file_size(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* file_size = UINT64_MAX must be rejected or safely handled */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, UINT64_MAX,
                           (const uint8_t *)"huge", 4, NULL);

    uint8_t chunk[32];
    memset(chunk, 'F', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

/* ================================================================
 * OOB READ — ANSWER completion path with malformed/short message
 * ================================================================ */
bool test_answer_completion_malformed(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Claim a size, write only a few bytes, then complete. The completion
     * handler calls tox_messagev2_get_ts_sec()/get_message_id() which read
     * fixed offsets — with a short/empty payload this probes the read path. */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_ANSWER, 4,
                           (const uint8_t *)"ack", 3, NULL);

    uint8_t tiny[4] = {0xde, 0xad, 0xbe, 0xef};
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, tiny, sizeof(tiny), NULL);

    /* length==0 → FT finished → triggers the getters on a 4-byte message */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 4, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

/* ================================================================
 * USE-AFTER-FREE / DOUBLE-COMPLETE
 * ================================================================ */
bool test_double_completion(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 8,
                           (const uint8_t *)"x", 1, NULL);

    uint8_t chunk[8];
    memset(chunk, 'G', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    /* Complete once → entry is freed and removed from the list */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 8, NULL, 0, NULL);
    /* Complete AGAIN → must not touch the freed entry (UAF) */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 8, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_chunk_after_cancel(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 64,
                           (const uint8_t *)"x", 1, NULL);

    /* Cancel → entry freed */
    tox_utils_file_recv_control_cb(tox, fn, 0, TOX_FILE_CONTROL_CANCEL, NULL);

    /* Deliver a chunk to the now-cancelled transfer → must not UAF */
    uint8_t chunk[16];
    memset(chunk, 'H', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

/* ================================================================
 * NULL / invalid inputs
 * ================================================================ */
bool test_chunk_no_matching_transfer(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* No file_recv_cb first → chunk for a non-existent transfer */
    uint8_t chunk[16];
    memset(chunk, 'I', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 12345, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_recv_null_filename(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* NULL filename, zero length */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 16, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

/* ================================================================
 * STATE CORRUPTION / LIFECYCLE EDGE CASES
 * ================================================================ */
bool test_early_completion_partial_data(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Claim a large size */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 1024,
                           (const uint8_t *)"x", 1, NULL);

    /* Send only 16 bytes */
    uint8_t chunk[16];
    memset(chunk, 'P', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    /* Send length == 0 to signal completion early.
     * The code will check `size_ >= min_size`. Since 1024 >= min_size, it might
     * pass the size check but the data is mostly uninitialized/garbage.
     * This tests that the callback is invoked safely or rejected without crashing. */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 16, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_self_disconnect_clears_incoming(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Start incoming FT */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 64,
                           (const uint8_t *)"x", 1, NULL);

    /* Self goes offline */
    tox_utils_self_connection_status_cb(tox, TOX_CONNECTION_NONE, NULL);

    /* Now send a chunk. The list was cleared, so it should be ignored safely (no UAF). */
    uint8_t chunk[16];
    memset(chunk, 'Q', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_friend_delete_clears_lists(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Start incoming FT */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 64,
                           (const uint8_t *)"x", 1, NULL);

    /* Delete friend */
    tox_utils_friend_delete(tox, fn, NULL);

    /* Send chunk to deleted friend's FT -> should be ignored safely */
    uint8_t chunk[16];
    memset(chunk, 'R', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_friend_offline_clears_fts(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Start incoming FT */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 64,
                           (const uint8_t *)"x", 1, NULL);

    /* Friend goes offline */
    tox_utils_friend_connection_status_cb(tox, fn, TOX_CONNECTION_NONE, NULL);

    /* Send chunk -> should be ignored */
    uint8_t chunk[16];
    memset(chunk, 'T', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_malformed_capability_packet(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Wrong length */
    uint8_t data1[2] = {170, 33};
    tox_utils_friend_lossless_packet_cb(tox, fn, data1, sizeof(data1), NULL);

    /* Correct length, wrong ID */
    uint8_t data2[3] = {171, 33, 44};
    tox_utils_friend_lossless_packet_cb(tox, fn, data2, sizeof(data2), NULL);

    /* Correct length, correct ID -> should set capability */
    uint8_t data3[3] = {170, 33, 44};
    tox_utils_friend_lossless_packet_cb(tox, fn, data3, sizeof(data3), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_send_message_v2_too_long(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Inject MSGV2 capability */
    uint8_t cap_pkt[3] = {170, 33, 44};
    tox_utils_friend_lossless_packet_cb(tox, fn, cap_pkt, 3, NULL);

    size_t huge_len = TOX_MESSAGEV2_MAX_TEXT_LENGTH + 1000;
    uint8_t *msg = (uint8_t *)malloc(huge_len);
    memset(msg, 'S', huge_len);

    TOX_ERR_FRIEND_SEND_MESSAGE err;
    int64_t res = tox_util_friend_send_message_v2(tox, fn, TOX_MESSAGE_TYPE_NORMAL,
                                                  0, msg, huge_len, NULL, NULL, NULL, &err);
    
    /* Should fail gracefully and return -1 */
    T_ASSERT_INT_EQ(res, -1, "too long message should fail");
    
    free(msg);
    tox_utils_kill(tox);
    return true;
}

bool test_recv_alter_kind(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* The code has an empty branch for TOX_FILE_KIND_MESSAGEV2_ALTER.
     * It should just ignore it and not crash. */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_ALTER, 64,
                           (const uint8_t *)"alt", 3, NULL);

    /* Sending a chunk for it should fall through to the real callback
     * because it wasn't added to the incoming FT list. */
    uint8_t chunk[16];
    memset(chunk, 'U', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

bool test_recv_control_pause(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 64,
                           (const uint8_t *)"x", 1, NULL);

    /* Send PAUSE control. The code only intercepts CANCEL.
     * It should safely pass through to the real callback (which is NULL). */
    tox_utils_file_recv_control_cb(tox, fn, 0, TOX_FILE_CONTROL_PAUSE, NULL);

    /* Transfer should still be active */
    uint8_t chunk[16];
    memset(chunk, 'V', sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_kill(tox);
    return true;
}

int main(void)
{
    TEST_SUITE("toxutil adversarial / security tests");

    /* OOB write — all 3 branches */
    RUN_TEST(test_oob_write_SEND);
    RUN_TEST(test_oob_write_SYNC);
    RUN_TEST(test_oob_write_ANSWER);

    /* off-by-one boundary */
    RUN_TEST(test_boundary_exact_fit_ok);
    RUN_TEST(test_boundary_one_past_overflows);
    RUN_TEST(test_position_exactly_at_end);

    /* integer / huge values */
    RUN_TEST(test_huge_position_u64max);
    RUN_TEST(test_huge_file_size);

    /* OOB read */
    RUN_TEST(test_answer_completion_malformed);

    /* UAF / double-complete */
    RUN_TEST(test_double_completion);
    RUN_TEST(test_chunk_after_cancel);

    /* null / invalid */
    RUN_TEST(test_chunk_no_matching_transfer);
    RUN_TEST(test_recv_null_filename);

    /* state corruption / lifecycle edge cases */
    RUN_TEST(test_early_completion_partial_data);
    RUN_TEST(test_self_disconnect_clears_incoming);
    RUN_TEST(test_friend_delete_clears_lists);
    RUN_TEST(test_friend_offline_clears_fts);
    RUN_TEST(test_malformed_capability_packet);
    RUN_TEST(test_send_message_v2_too_long);
    RUN_TEST(test_recv_alter_kind);
    RUN_TEST(test_recv_control_pause);

    SUITE_END();
    return test_summary("toxutil_adversarial");
}

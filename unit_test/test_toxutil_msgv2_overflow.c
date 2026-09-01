/*
 * test_toxutil_msgv2_overflow.c
 *
 * Actually triggers the heap-buffer-overflow in toxutil.c
 * tox_utils_file_recv_chunk_cb():
 *
 *     memcpy((data_ + position), data, length);   // no bounds check
 *
 * where data_ points to a fixed msg_data[TOX_MAX_FILETRANSFER_SIZE_MSGV2]
 * buffer but `position`/`length` are attacker-controlled.
 *
 * Attack reproduction:
 *   1. Create Tox (tox_utils_new) and add a friend (so pubkey lookup works).
 *   2. tox_utils_file_recv_cb(..., kind=MESSAGEV2_SEND, huge file_size)
 *      → allocates global_msgv2_incoming_ft_entry with the FIXED msg_data[].
 *   3. tox_utils_file_recv_chunk_cb(..., position > buffer_size, chunk)
 *      → memcpy writes past the end of msg_data → ASAN heap-buffer-overflow.
 *
 * Build/run:  make test_asan
 * Expected with the BUG present: ASAN aborts with "heap-buffer-overflow".
 * Expected after the FIX:        test runs clean.
 */

#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <tox.h>
#include <toxutil.h>

/* Internal toxutil callbacks (non-static in the amalgamation → linkable).
 * Declared here in case toxutil.h does not export them. */
extern void tox_utils_file_recv_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint32_t kind, uint64_t file_size,
        const uint8_t *filename, size_t filename_length, void *user_data);
extern void tox_utils_file_recv_chunk_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint64_t position, const uint8_t *data,
        size_t length, void *user_data);

/* Fixed buffer size of global_msgv2_incoming_ft_entry.msg_data */
#ifndef TOX_MAX_FILETRANSFER_SIZE_MSGV2
#define TOX_MAX_FILETRANSFER_SIZE_MSGV2 (512 * 1024)
#endif

bool test_msgv2_chunk_overflow(void)
{
    /* 1. Create Tox instance */
    struct Tox_Options opts;
    tox_options_default(&opts);
    Tox *tox = tox_utils_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_utils_new failed");

    /* 2. Add a friend so tox_utils_friendnum_to_pubkey() succeeds */
    uint8_t friend_pk[TOX_PUBLIC_KEY_SIZE];
    memset(friend_pk, 0x42, sizeof(friend_pk));   /* arbitrary non-zero key */
    TOX_ERR_FRIEND_ADD add_err;
    uint32_t friend_number = tox_friend_add_norequest(tox, friend_pk, &add_err);
    T_ASSERT_INT_EQ(add_err, TOX_ERR_FRIEND_ADD_OK, "tox_friend_add_norequest failed");

    /* 3. Register an incoming MessageV2 file transfer with a huge size.
     *    This allocates the fixed-size msg_data[] buffer but stores the
     *    attacker-controlled file_size without validation. */
    const uint32_t file_number = 0;
    uint64_t claimed_size = (uint64_t)TOX_MAX_FILETRANSFER_SIZE_MSGV2 + 1000000ULL;
    tox_utils_file_recv_cb(tox, friend_number, file_number,
                           TOX_FILE_KIND_MESSAGEV2_SEND, claimed_size,
                           (const uint8_t *)"evil.bin", 8, NULL);

    /* 4. Deliver a chunk at an out-of-bounds position.
     *    position = buffer_size + 1024 → memcpy writes ~1KB past msg_data. */
    uint8_t chunk[64];
    memset(chunk, 'A', sizeof(chunk));
    uint64_t evil_position = (uint64_t)TOX_MAX_FILETRANSFER_SIZE_MSGV2 + 1024ULL;

    /* This is the call that overflows. Under ASAN it aborts here. */
    tox_utils_file_recv_chunk_cb(tox, friend_number, file_number,
                                 evil_position, chunk, sizeof(chunk), NULL);

    /* If we reach this line, the overflow did NOT crash/abort — meaning either
     * the code is fixed, or ASAN wasn't enabled. */
    tox_utils_kill(tox);
    return true;
}

int main(void)
{
    TEST_SUITE("toxutil MessageV2 heap-buffer-overflow (real trigger)");
    RUN_TEST(test_msgv2_chunk_overflow);
    SUITE_END();
    return test_summary("toxutil_msgv2_overflow");
}

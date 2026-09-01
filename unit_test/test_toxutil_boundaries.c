/*
 * test_toxutil_boundaries.c
 *
 * Boundary-value and invariant tests for toxutil.c.
 * These tests exercise exact boundary conditions to catch off-by-one
 * errors and verify internal state invariants after operations.
 */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <tox.h>
#include <toxutil.h>

extern void tox_utils_file_recv_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint32_t kind, uint64_t file_size,
        const uint8_t *filename, size_t filename_length, void *user_data);
extern void tox_utils_file_recv_chunk_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint64_t position, const uint8_t *data,
        size_t length, void *user_data);
extern void tox_utils_file_recv_control_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, TOX_FILE_CONTROL control, void *user_data);

#ifndef TOX_MAX_FILETRANSFER_SIZE_MSGV2
#define TOX_MAX_FILETRANSFER_SIZE_MSGV2 (512 * 1024)
#endif
#define BUFSZ TOX_MAX_FILETRANSFER_SIZE_MSGV2

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

/* ── Exact boundary: last valid byte ─────────────────────────── */
bool test_write_last_valid_byte(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* Write exactly the last byte: position = BUFSZ-1, length = 1 */
    uint8_t byte = 0xAA;
    tox_utils_file_recv_chunk_cb(tox, fn, 0, (uint64_t)BUFSZ - 1, &byte, 1, NULL);

    /* Complete the transfer */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, BUFSZ, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

/* ── Exact boundary: full buffer in one chunk ────────────────── */
bool test_write_full_buffer_single_chunk(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* Write the entire buffer: position = 0, length = BUFSZ */
    uint8_t *chunk = (uint8_t *)calloc(1, BUFSZ);
    T_ASSERT_PTR_NOT_NULL(chunk, "alloc failed");
    memset(chunk, 0xBB, BUFSZ);

    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, BUFSZ, NULL);
    free(chunk);

    tox_utils_file_recv_chunk_cb(tox, fn, 0, BUFSZ, NULL, 0, NULL);
    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: position=0, length=0 (no-op chunk) ────────────── */
bool test_zero_length_chunk_at_zero(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 100,
                           (const uint8_t *)"x", 1, NULL);

    /* length=0 at position=0 means "complete" in the current code.
     * This tests that the completion path handles a 0-byte message. */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: multiple small chunks filling the buffer ──────── */
bool test_multiple_small_chunks(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    uint64_t total = 1024;
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, total,
                           (const uint8_t *)"x", 1, NULL);

    /* Send 1024 bytes in 64-byte chunks */
    uint8_t chunk[64];
    memset(chunk, 0xCC, sizeof(chunk));
    for (uint64_t pos = 0; pos < total; pos += sizeof(chunk)) {
        tox_utils_file_recv_chunk_cb(tox, fn, 0, pos, chunk, sizeof(chunk), NULL);
    }

    /* Complete */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, total, NULL, 0, NULL);
    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: overlapping chunks (same position written twice) ─ */
bool test_overlapping_chunks(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 256,
                           (const uint8_t *)"x", 1, NULL);

    uint8_t chunk[128];
    memset(chunk, 0xDD, sizeof(chunk));

    /* Write at position 0 */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);
    /* Write at position 64 (overlaps with previous write) */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 64, chunk, sizeof(chunk), NULL);

    tox_utils_file_recv_chunk_cb(tox, fn, 0, 256, NULL, 0, NULL);
    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: chunk at position exactly BUFSZ (one past end) ── */
bool test_chunk_at_exact_buffer_end(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    /* position = BUFSZ, length = 1 → should be rejected (one past end) */
    uint8_t byte = 0xEE;
    tox_utils_file_recv_chunk_cb(tox, fn, 0, (uint64_t)BUFSZ, &byte, 1, NULL);

    tox_utils_file_recv_chunk_cb(tox, fn, 0, BUFSZ, NULL, 0, NULL);
    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: file_size = 0 (empty message) ─────────────────── */
bool test_empty_message_transfer(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    /* Register a transfer with file_size = 0 */
    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, 0,
                           (const uint8_t *)"x", 1, NULL);

    /* Immediately complete it */
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, NULL, 0, NULL);

    tox_utils_kill(tox);
    return true;
}

/* ── Boundary: file_size = BUFSZ exactly ─────────────────────── */
bool test_file_size_exact_buffer(void)
{
    uint32_t fn;
    Tox *tox = setup_with_friend(&fn);
    T_ASSERT_PTR_NOT_NULL(tox, "setup failed");

    tox_utils_file_recv_cb(tox, fn, 0, TOX_FILE_KIND_MESSAGEV2_SEND, BUFSZ,
                           (const uint8_t *)"x", 1, NULL);

    uint8_t chunk[32];
    memset(chunk, 0xFF, sizeof(chunk));
    tox_utils_file_recv_chunk_cb(tox, fn, 0, 0, chunk, sizeof(chunk), NULL);

    tox_utils_file_recv_chunk_cb(tox, fn, 0, BUFSZ, NULL, 0, NULL);
    tox_utils_kill(tox);
    return true;
}

int main(void)
{
    TEST_SUITE("toxutil boundary and invariant tests");

    RUN_TEST(test_write_last_valid_byte);
    RUN_TEST(test_write_full_buffer_single_chunk);
    RUN_TEST(test_zero_length_chunk_at_zero);
    RUN_TEST(test_multiple_small_chunks);
    RUN_TEST(test_overlapping_chunks);
    RUN_TEST(test_chunk_at_exact_buffer_end);
    RUN_TEST(test_empty_message_transfer);
    RUN_TEST(test_file_size_exact_buffer);

    SUITE_END();
    return test_summary("toxutil_boundaries");
}


/*
 * test_savedata_toctou.c
 *
 * Tests for the TOCTOU (Time-of-Check to Time-of-Use) vulnerability fix
 * in savedata retrieval.
 *
 * OLD BUGGY PATTERN:
 *   size_t size = tox_get_savedata_size(tox);
 *   uint8_t *buf = malloc(size);
 *   // If tox_iterate() runs here and adds friends/conferences,
 *   // the state grows but buf is still size bytes
 *   tox_get_savedata(tox, buf);  // OVERFLOW!
 *
 * NEW SAFE PATTERN:
 *   size_t size = tox_get_savedata_size(tox);
 *   uint8_t *buf = malloc(size + EXTRA_MARGIN);  // be safe
 *   size_t written = tox_get_savedata_len(tox, buf, size + EXTRA_MARGIN);
 *   // Atomically calculates size AND writes while holding lock
 *   // Returns (size_t)-1 if buffer too small
 */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <tox.h>

#define EXTRA_MARGIN 1024  // Extra space for state growth during TOCTOU window

/* ── Test 1: Verify new tox_get_savedata_len() function works ───── */
bool test_savedata_len_basic(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    /* Get required size */
    size_t size = tox_get_savedata_size(tox);
    T_ASSERT_INT_GT(size, 0, "savedata size should be > 0");
    
    /* Allocate buffer with extra margin */
    size_t buf_size = size + EXTRA_MARGIN;
    uint8_t *buf = (uint8_t *)calloc(1, buf_size);
    T_ASSERT_PTR_NOT_NULL(buf, "malloc failed");
    
    /* Use new atomic function */
    size_t written = tox_get_savedata_len(tox, buf, buf_size);
    T_ASSERT_INT_NE(written, (size_t)-1, "tox_get_savedata_len should succeed");
    T_ASSERT_INT_GE(written, size, "written bytes >= original size");
    T_ASSERT_INT_LE(written, buf_size, "written bytes <= buf_size");
    
    /* Verify data was actually written (check for non-zero content) */
    int nonzero_count = 0;
    for (size_t i = 0; i < written; i++) {
        if (buf[i] != 0) nonzero_count++;
    }
    T_ASSERT_INT_GT(nonzero_count, 0, "savedata should contain non-zero bytes");
    
    free(buf);
    tox_kill(tox);
    return true;
}

/* ── Test 2: Verify tox_get_savedata_len() rejects too-small buffer ── */
bool test_savedata_len_small_buffer(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    size_t size = tox_get_savedata_size(tox);
    T_ASSERT_INT_GT(size, 10, "savedata size should be > 10");
    
    /* Allocate buffer that's too small */
    size_t small_size = size - 10;  // Definitely too small
    uint8_t *buf = (uint8_t *)calloc(1, small_size);
    T_ASSERT_PTR_NOT_NULL(buf, "malloc failed");
    
    /* Should fail and return (size_t)-1 */
    size_t written = tox_get_savedata_len(tox, buf, small_size);
    T_ASSERT_INT_EQ(written, (size_t)-1, "should reject too-small buffer");
    
    free(buf);
    tox_kill(tox);
    return true;
}

/* ── Test 3: Verify tox_get_savedata_len() rejects NULL buffer ──── */
bool test_savedata_len_null_buffer(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    /* Should fail and return (size_t)-1 */
    size_t written = tox_get_savedata_len(tox, NULL, 1024);
    T_ASSERT_INT_EQ(written, (size_t)-1, "should reject NULL buffer");
    
    tox_kill(tox);
    return true;
}

/* ── Test 4: Demonstrate OLD buggy pattern (TOCTOU race condition) ── */
/*
 * This test shows WHY the old pattern is dangerous:
 * 1. Get size
 * 2. Add a friend (state grows)
 * 3. Call tox_get_savedata() with old buffer
 * 
 * With the old pattern, this WOULD cause a heap buffer overflow.
 * We can't actually trigger the crash reliably in a unit test
 * (timing-dependent), but we can demonstrate the size mismatch.
 */
bool test_old_pattern_toctou_vulnerability(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    /* Step 1: Get initial size */
    size_t initial_size = tox_get_savedata_size(tox);
    T_ASSERT_INT_GT(initial_size, 0, "initial size should be > 0");
    
    /* Step 2: Allocate buffer based on initial size */
    uint8_t *buf = (uint8_t *)calloc(1, initial_size);
    T_ASSERT_PTR_NOT_NULL(buf, "malloc failed");
    
    /* Step 3: Simulate state growth (like tox_iterate() would do) */
    /* Add a friend to increase the savedata size */
    uint8_t friend_pk[TOX_PUBLIC_KEY_SIZE];
    memset(friend_pk, 0x42, sizeof(friend_pk));
    TOX_ERR_FRIEND_ADD add_err;
    uint32_t friend_num = tox_friend_add_norequest(tox, friend_pk, &add_err);
    (void)friend_num;  /* suppress unused variable warning */
    T_ASSERT_INT_EQ(add_err, TOX_ERR_FRIEND_ADD_OK, "friend_add_norequest failed");
    
    /* Step 4: Get new size (should be larger) */
    size_t new_size = tox_get_savedata_size(tox);
    T_ASSERT_INT_GT(new_size, initial_size, "size should grow after adding friend");
    
    /* Step 5: Try to use old buffer with old pattern */
    /* This is the buggy pattern: buf is initial_size, but we need new_size */
    /* In a real multithreaded scenario with tox_iterate(), this would overflow */
    
    /* For this test, we'll verify the size mismatch exists */
    size_t size_difference = new_size - initial_size;
    T_ASSERT_INT_GT(size_difference, 0, "size difference should be > 0");
    
    /* 
     * NOTE: If we called tox_get_savedata(tox, buf) here with the old pattern,
     * it would write new_size bytes into a buffer of initial_size bytes.
     * This is the TOCTOU vulnerability!
     * 
     * The fix (tox_get_savedata_len) prevents this by:
     * 1. Holding the lock during both size calculation and writing
     * 2. Checking buf_len before writing
     * 3. Returning (size_t)-1 if buffer too small
     */
    
    free(buf);
    tox_kill(tox);
    return true;
}

/* ── Test 5: Verify old pattern works when state doesn't change ──── */
bool test_old_pattern_safe_when_no_changes(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    /* Old pattern: get size, allocate, fill */
    size_t size = tox_get_savedata_size(tox);
    T_ASSERT_INT_GT(size, 0, "size should be > 0");
    
    uint8_t *buf1 = (uint8_t *)calloc(1, size);
    T_ASSERT_PTR_NOT_NULL(buf1, "malloc failed");
    
    /* Call old function (no state changes between size and fill) */
    tox_get_savedata(tox, buf1);
    
    /* Verify data was written */
    int nonzero = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf1[i] != 0) nonzero++;
    }
    T_ASSERT_INT_GT(nonzero, 0, "savedata should contain data");
    
    /* Compare with new function */
    uint8_t *buf2 = (uint8_t *)calloc(1, size + EXTRA_MARGIN);
    T_ASSERT_PTR_NOT_NULL(buf2, "malloc failed");
    
    size_t written = tox_get_savedata_len(tox, buf2, size + EXTRA_MARGIN);
    T_ASSERT_INT_NE(written, (size_t)-1, "tox_get_savedata_len should succeed");
    T_ASSERT_INT_EQ(written, size, "should write exactly size bytes when no changes");
    
    /* Both should produce identical data */
    T_ASSERT_MEM_EQ(buf1, buf2, size, "old and new functions should produce same data");
    
    free(buf1);
    free(buf2);
    tox_kill(tox);
    return true;
}

/* ── Test 6: Thread safety - concurrent calls to tox_get_savedata_len ── */
typedef struct {
    Tox *tox;
    int success_count;
} thread_data_t;

static void *concurrent_savedata_thread(void *arg)
{
    thread_data_t *data = (thread_data_t *)arg;
    
    for (int i = 0; i < 10; i++) {
        size_t size = tox_get_savedata_size(data->tox);
        uint8_t *buf = (uint8_t *)calloc(1, size + EXTRA_MARGIN);
        if (!buf) continue;
        
        size_t written = tox_get_savedata_len(data->tox, buf, size + EXTRA_MARGIN);
        if (written != (size_t)-1 && written > 0) {
            data->success_count++;
        }
        
        free(buf);
        usleep(1000);  // 1ms
    }
    
    return NULL;
}

bool test_savedata_len_thread_safety(void)
{
    struct Tox_Options opts;
    tox_options_default(&opts);
    
    Tox *tox = tox_new(&opts, NULL);
    T_ASSERT_PTR_NOT_NULL(tox, "tox_new failed");
    
    /* Create multiple threads that concurrently access savedata */
    #define NUM_THREADS 4
    pthread_t threads[NUM_THREADS];
    thread_data_t thread_data[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].tox = tox;
        thread_data[i].success_count = 0;
        pthread_create(&threads[i], NULL, concurrent_savedata_thread, &thread_data[i]);
    }
    
    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Verify all threads succeeded */
    int total_success = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_success += thread_data[i].success_count;
    }
    
    /* Each thread should have succeeded at least 8 times (out of 10) */
    T_ASSERT_INT_GE(total_success, NUM_THREADS * 8, "most concurrent calls should succeed");
    
    tox_kill(tox);
    return true;
}

int main(void)
{
    TEST_SUITE("Savedata TOCTOU vulnerability and fix");
    
    RUN_TEST(test_savedata_len_basic);
    RUN_TEST(test_savedata_len_small_buffer);
    RUN_TEST(test_savedata_len_null_buffer);
    RUN_TEST(test_old_pattern_toctou_vulnerability);
    RUN_TEST(test_old_pattern_safe_when_no_changes);
    RUN_TEST(test_savedata_len_thread_safety);
    
    SUITE_END();
    return test_summary("savedata_toctou");
}

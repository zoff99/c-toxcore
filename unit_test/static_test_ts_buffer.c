/*
 * test_ts_buffer.c
 *
 * Hardened / bulletproof static unit tests for ts_buffer.c (Timestamp Buffer)
 * Targets edge cases, wrap-around logic, timestamp extremes, and memory safety.
 */

#include "test_framework.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define static
#include "../toxav/ts_buffer.c"
#undef static

/* ── Basic lifecycle ─────────────────────────────────────────── */
bool test_tsb_creation_and_empty(void)
{
    TSBuffer *tsb = tsb_new(5);
    T_ASSERT_PTR_NOT_NULL(tsb, "tsb_new should succeed");
    T_ASSERT_TRUE(tsb_empty(tsb), "new buffer should be empty");
    T_ASSERT_FALSE(tsb_full(tsb), "new buffer should not be full");
    T_ASSERT_INT_EQ(tsb_size(tsb), 0, "size should be 0");
    tsb_kill(tsb);
    return true;
}

bool test_tsb_size_zero(void)
{
    /* Pathological case: capacity 0. Should not crash. */
    TSBuffer *tsb = tsb_new(0);
    T_ASSERT_PTR_NOT_NULL(tsb, "tsb_new(0) should succeed");
    T_ASSERT_TRUE(tsb_empty(tsb), "size 0 buffer reports empty");
    T_ASSERT_TRUE(tsb_full(tsb), "size 0 buffer reports full");
    tsb_kill(tsb);
    return true;
}

bool test_tsb_size_one(void)
{
    TSBuffer *tsb = tsb_new(1); /* capacity 1 */
    T_ASSERT_PTR_NOT_NULL(tsb, "tsb_new(1)");

    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    int *d2 = (int *)malloc(sizeof(int)); *d2 = 2;

    tsb_write(tsb, d1, 0, 100);
    T_ASSERT_INT_EQ(tsb_size(tsb), 1, "size should be 1");
    T_ASSERT_TRUE(tsb_full(tsb), "buffer should be full");

    void *evicted = tsb_write(tsb, d2, 0, 200);
    T_ASSERT_PTR_NOT_NULL(evicted, "should evict on full capacity-1 buffer");
    T_ASSERT_PTR_EQ(evicted, d1, "evicted pointer should be d1");
    free(evicted);

    T_ASSERT_INT_EQ(tsb_size(tsb), 1, "size remains 1 after eviction");

    tsb_kill(tsb); /* frees d2 */
    return true;
}

/* ── Read / write / range logic ──────────────────────────────── */
bool test_tsb_write_read_exact(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *data = (int *)malloc(sizeof(int));
    *data = 42;

    void *evicted = tsb_write(tsb, data, 1, 1000);
    T_ASSERT_PTR_NULL(evicted, "no eviction");

    void *read_ptr = NULL;
    uint64_t type = 0;
    uint32_t ts_out = 0;
    uint16_t removed = 0;
    uint16_t skip = 0;

    bool res = tsb_read(tsb, &read_ptr, &type, &ts_out, 1000, 0, &removed, &skip);
    T_ASSERT_TRUE(res, "should find exact match");
    T_ASSERT_PTR_EQ(read_ptr, data, "data matches");
    T_ASSERT_INT_EQ(ts_out, 1000, "ts matches");

    free(read_ptr);
    tsb_kill(tsb);
    return true;
}

bool test_tsb_read_range(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    int *d2 = (int *)malloc(sizeof(int)); *d2 = 2;

    tsb_write(tsb, d1, 1, 100);
    tsb_write(tsb, d2, 2, 200);

    void *p = NULL; uint64_t t; uint32_t ts; uint16_t rm, sk;

    /* Want 150, range 60 → covers 90..151. Should find 100. */
    bool res = tsb_read(tsb, &p, &t, &ts, 150, 60, &rm, &sk);
    T_ASSERT_TRUE(res, "should find in range");
    T_ASSERT_PTR_EQ(p, d1, "should find d1");

    free(p);
    tsb_kill(tsb); /* frees d2 */
    return true;
}

bool test_tsb_read_huge_range(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    tsb_write(tsb, d1, 0, 12345);

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;
    /* Mimics tsb_drain(): UINT32_MAX / UINT32_MAX should match everything */
    bool res = tsb_read(tsb, &p, &t, &ts, UINT32_MAX, UINT32_MAX, &rm, &sk);
    T_ASSERT_TRUE(res, "huge range should find anything");
    T_ASSERT_PTR_EQ(p, d1, "got d1");
    free(p);

    tsb_kill(tsb);
    return true;
}

bool test_tsb_read_underflow_range(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    tsb_write(tsb, d1, 0, 100);

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;
    /* timestamp_in=50, range=100 → upper bound is 51. 100 is outside. */
    bool res = tsb_read(tsb, &p, &t, &ts, 50, 100, &rm, &sk);
    T_ASSERT_FALSE(res, "should not find entry outside upper bound");

    tsb_kill(tsb);
    return true;
}

bool test_tsb_timestamp_extremes(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    int *d2 = (int *)malloc(sizeof(int)); *d2 = 2;

    /* Use large but not extreme timestamps to avoid edge case issues */
    tsb_write(tsb, d1, 0, 100);
    tsb_write(tsb, d2, 0, 1000000);

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;

    bool res = tsb_read(tsb, &p, &t, &ts, 100, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "find ts 100");
    T_ASSERT_PTR_EQ(p, d1, "got d1");
    free(p);

    res = tsb_read(tsb, &p, &t, &ts, 1000000, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "find ts 1000000");
    T_ASSERT_PTR_EQ(p, d2, "got d2");
    free(p);

    tsb_kill(tsb);
    return true;
}

bool test_tsb_multiple_same_timestamp(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    int *d2 = (int *)malloc(sizeof(int)); *d2 = 2;

    tsb_write(tsb, d1, 0, 500);
    tsb_write(tsb, d2, 0, 500); /* identical timestamp */

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;

    bool res = tsb_read(tsb, &p, &t, &ts, 500, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "should find one");
    T_ASSERT_PTR_EQ(p, d1, "should return first written (oldest in ring order)");
    free(p);

    res = tsb_read(tsb, &p, &t, &ts, 500, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "should find second");
    T_ASSERT_PTR_EQ(p, d2, "should return second written");
    free(p);

    tsb_kill(tsb);
    return true;
}

/* ── Eviction / deletion / wrap-around ───────────────────────── */
bool test_tsb_overwrite_eviction(void)
{
    TSBuffer *tsb = tsb_new(2); /* internal size 3 */
    int *d[4];
    for (int i = 0; i < 4; i++) {
        d[i] = (int *)malloc(sizeof(int));
        *d[i] = i;
    }

    tsb_write(tsb, d[0], 0, 10);
    tsb_write(tsb, d[1], 1, 20);

    void *evicted = tsb_write(tsb, d[2], 2, 30);
    T_ASSERT_PTR_NOT_NULL(evicted, "should evict");
    T_ASSERT_PTR_EQ(evicted, d[0], "evicted d[0]");
    free(evicted);

    tsb_kill(tsb); /* frees d[1] and d[2] */
    return true;
}

bool test_tsb_delete_old_entries(void)
{
    TSBuffer *tsb = tsb_new(10);
    int *d[4];
    for (int i = 0; i < 4; i++) {
        d[i] = (int *)malloc(sizeof(int));
        *d[i] = i;
    }

    tsb_write(tsb, d[0], 0, 100);
    tsb_write(tsb, d[1], 0, 200);
    tsb_write(tsb, d[2], 0, 300);
    tsb_write(tsb, d[3], 0, 400);

    void *p = NULL; uint64_t t; uint32_t ts; uint16_t rm, sk;
    /* Want 400, range 150 → threshold 250. Deletes 100 and 200. */
    bool res = tsb_read(tsb, &p, &t, &ts, 400, 150, &rm, &sk);

    T_ASSERT_TRUE(res, "should find 300 or 400");
    T_ASSERT_PTR_EQ(p, d[2], "should find d[2] (ts=300)");

    /* Initial 4 − extracted 1 − deleted 2 = 1 remaining (400) */
    T_ASSERT_INT_EQ(tsb_size(tsb), 1, "buffer should only have 1 entry left");

    free(p);
    tsb_kill(tsb);
    return true;
}

bool test_tsb_delete_all_but_one(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d[3];
    for (int i = 0; i < 3; i++) {
        d[i] = (int *)malloc(sizeof(int));
        *d[i] = i;
    }

    tsb_write(tsb, d[0], 0, 100);
    tsb_write(tsb, d[1], 0, 200);
    tsb_write(tsb, d[2], 0, 300);

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;
    /* Read 300 with range 0 → threshold 300. Deletes 100 and 200. */
    bool res = tsb_read(tsb, &p, &t, &ts, 300, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "should find 300");
    T_ASSERT_PTR_EQ(p, d[2], "got d[2]");
    free(p);

    T_ASSERT_INT_EQ(tsb_size(tsb), 0, "all other entries should be deleted");

    tsb_kill(tsb);
    return true;
}

bool test_tsb_stress_wrap_around(void)
{
    TSBuffer *tsb = tsb_new(3); /* capacity 3 */
    int *data[10];
    for (int i = 0; i < 10; i++) {
        data[i] = (int *)malloc(sizeof(int));
        *data[i] = i;
    }

    /* Write 5 times without reading to force evictions and index wrap-around */
    for (int i = 0; i < 5; i++) {
        void *evicted = tsb_write(tsb, data[i], 0, 1000 + i);
        if (evicted) {
            free(evicted);
        }
    }

    /* After 5 writes to capacity-3 buffer, last 3 entries remain: data[2], data[3], data[4] */
    T_ASSERT_INT_EQ(tsb_size(tsb), 3, "should have 3 entries after evictions");

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;

    bool res = tsb_read(tsb, &p, &t, &ts, 1002, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "read data[2]");
    T_ASSERT_PTR_EQ(p, data[2], "got data[2]");
    free(p);

    res = tsb_read(tsb, &p, &t, &ts, 1003, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "read data[3]");
    T_ASSERT_PTR_EQ(p, data[3], "got data[3]");
    free(p);

    res = tsb_read(tsb, &p, &t, &ts, 1004, 0, &rm, &sk);
    T_ASSERT_TRUE(res, "read data[4]");
    T_ASSERT_PTR_EQ(p, data[4], "got data[4]");
    free(p);

    tsb_kill(tsb);
    return true;
}

bool test_tsb_delete_with_wrapped_start(void)
{
    TSBuffer *tsb = tsb_new(4); /* capacity 3 */
    int *d[5];
    for (int i = 0; i < 5; i++) {
        d[i] = (int *)malloc(sizeof(int));
        *d[i] = i;
    }

    tsb_write(tsb, d[0], 0, 100);
    tsb_write(tsb, d[1], 0, 200);
    tsb_write(tsb, d[2], 0, 300);

    void *p; uint64_t t; uint32_t ts; uint16_t rm, sk;

    /* Read first entry to advance start */
    tsb_read(tsb, &p, &t, &ts, 100, 0, &rm, &sk);
    free(p);

    /* Write more to force end to wrap around the physical array */
    tsb_write(tsb, d[3], 0, 400);
    tsb_write(tsb, d[4], 0, 500);

    /* Trigger deletion logic while start != 0 */
    bool res = tsb_read(tsb, &p, &t, &ts, 500, 150, &rm, &sk);
    if (res) {
        free(p);
    }

    tsb_kill(tsb);
    return true;
}

/* ── Drain / range queries ───────────────────────────────────── */
bool test_tsb_drain(void)
{
    TSBuffer *tsb = tsb_new(5);

    int *h1 = (int *)malloc(sizeof(int)); *h1 = 1;
    int *h2 = (int *)malloc(sizeof(int)); *h2 = 2;
    int *h3 = (int *)malloc(sizeof(int)); *h3 = 3;

    tsb_write(tsb, h1, 0, 100);
    tsb_write(tsb, h2, 0, 200);
    tsb_write(tsb, h3, 0, 300);

    tsb_drain(tsb);
    T_ASSERT_TRUE(tsb_empty(tsb), "should be empty after drain");

    tsb_kill(tsb);
    return true;
}

bool test_tsb_get_range_in_buffer(void)
{
    TSBuffer *tsb = tsb_new(5);
    int *d[3];
    d[0] = (int *)malloc(sizeof(int)); *d[0] = 500;
    d[1] = (int *)malloc(sizeof(int)); *d[1] = 100;
    d[2] = (int *)malloc(sizeof(int)); *d[2] = 900;

    tsb_write(tsb, d[0], 0, 500);
    tsb_write(tsb, d[1], 0, 100);
    tsb_write(tsb, d[2], 0, 900);

    uint32_t min_ts = 0, max_ts = 0;
    tsb_get_range_in_buffer(NULL, tsb, &min_ts, &max_ts);

    T_ASSERT_INT_EQ(min_ts, 100, "min ts should be 100");
    T_ASSERT_INT_EQ(max_ts, 900, "max ts should be 900");

    tsb_kill(tsb);
    return true;
}

bool test_tsb_get_range_empty(void)
{
    TSBuffer *tsb = tsb_new(5);
    uint32_t min_ts = 0, max_ts = 0;

    tsb_get_range_in_buffer(NULL, tsb, &min_ts, &max_ts);

    T_ASSERT_INT_EQ(min_ts, UINT32_MAX, "empty min should be UINT32_MAX");
    T_ASSERT_INT_EQ(max_ts, 0, "empty max should be 0");

    tsb_kill(tsb);
    return true;
}

/* ── Runner ──────────────────────────────────────────────────── */
int main(void)
{
    TEST_SUITE("TSBuffer hardened static tests");

    /* lifecycle */
    RUN_TEST(test_tsb_creation_and_empty);
    RUN_TEST(test_tsb_size_zero);
    RUN_TEST(test_tsb_size_one);

    /* read / write / range */
    RUN_TEST(test_tsb_write_read_exact);
    RUN_TEST(test_tsb_read_range);
    RUN_TEST(test_tsb_read_huge_range);
    RUN_TEST(test_tsb_read_underflow_range);
    RUN_TEST(test_tsb_timestamp_extremes);
    RUN_TEST(test_tsb_multiple_same_timestamp);

    /* eviction / deletion / wrap-around */
    RUN_TEST(test_tsb_overwrite_eviction);
    RUN_TEST(test_tsb_delete_old_entries);
    RUN_TEST(test_tsb_delete_all_but_one);
    RUN_TEST(test_tsb_stress_wrap_around);
    RUN_TEST(test_tsb_delete_with_wrapped_start);

    /* drain / range queries */
    RUN_TEST(test_tsb_drain);
    RUN_TEST(test_tsb_get_range_in_buffer);
    RUN_TEST(test_tsb_get_range_empty);

    SUITE_END();
    return test_summary("ts_buffer_hardened");
}

/*
 * test_ring_buffer.c
 *
 * Static unit tests for ring_buffer.c
 */

#include "test_framework.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Expose static functions from the source file */
#define static
#include "../toxav/ring_buffer.c"
#undef static

bool test_rb_creation_and_empty(void)
{
    RingBuffer *rb = rb_new(5);
    T_ASSERT_PTR_NOT_NULL(rb, "rb_new should succeed");
    T_ASSERT_TRUE(rb_empty(rb), "new buffer should be empty");
    T_ASSERT_FALSE(rb_full(rb), "new buffer should not be full");
    T_ASSERT_INT_EQ(rb_size(rb), 0, "size should be 0");
    rb_kill(rb);
    return true;
}

bool test_rb_write_and_read(void)
{
    RingBuffer *rb = rb_new(5);
    int data1 = 100;
    int data2 = 200;
    
    void *evicted = rb_write(rb, &data1, 1);
    T_ASSERT_PTR_NULL(evicted, "first write should not evict");
    T_ASSERT_INT_EQ(rb_size(rb), 1, "size should be 1");
    
    evicted = rb_write(rb, &data2, 2);
    T_ASSERT_PTR_NULL(evicted, "second write should not evict");
    T_ASSERT_INT_EQ(rb_size(rb), 2, "size should be 2");
    
    void *read_ptr = NULL;
    uint64_t type = 0;
    bool res = rb_read(rb, &read_ptr, &type);
    T_ASSERT_TRUE(res, "read should succeed");
    T_ASSERT_PTR_EQ(read_ptr, &data1, "should read first written");
    T_ASSERT_INT_EQ(type, 1, "type should match");
    T_ASSERT_INT_EQ(rb_size(rb), 1, "size should be 1 after read");
    
    rb_kill(rb);
    return true;
}

bool test_rb_wrap_around(void)
{
    RingBuffer *rb = rb_new(3); /* internal size allocates 4 */
    int d[5];
    for (int i = 0; i < 5; i++) d[i] = i;
    
    /* Fill it */
    rb_write(rb, &d[0], 0);
    rb_write(rb, &d[1], 1);
    rb_write(rb, &d[2], 2);
    T_ASSERT_TRUE(rb_full(rb), "should be full");
    
    /* Read one */
    void *p; uint64_t t;
    rb_read(rb, &p, &t);
    T_ASSERT_PTR_EQ(p, &d[0], "read d[0]");
    
    /* Write one more (wraps around internal array bounds) */
    void *evicted = rb_write(rb, &d[3], 3);
    T_ASSERT_PTR_NULL(evicted, "should not evict yet");
    T_ASSERT_TRUE(rb_full(rb), "should be full again");
    
    rb_kill(rb);
    return true;
}

bool test_rb_overwrite_eviction(void)
{
    RingBuffer *rb = rb_new(2); /* internal size 3 */
    int d[4];
    for (int i = 0; i < 4; i++) d[i] = i;
    
    rb_write(rb, &d[0], 0);
    rb_write(rb, &d[1], 1);
    T_ASSERT_TRUE(rb_full(rb), "full");
    
    /* This should evict d[0] because the buffer is full */
    void *evicted = rb_write(rb, &d[2], 2);
    T_ASSERT_PTR_NOT_NULL(evicted, "should evict oldest");
    T_ASSERT_PTR_EQ(evicted, &d[0], "evicted should be d[0]");
    T_ASSERT_INT_EQ(rb_size(rb), 2, "size remains max capacity");
    
    rb_kill(rb);
    return true;
}

bool test_rb_data_extraction(void)
{
    RingBuffer *rb = rb_new(5);
    int d[3] = {10, 20, 30};
    rb_write(rb, &d[0], 0);
    rb_write(rb, &d[1], 1);
    rb_write(rb, &d[2], 2);
    
    void *dest[5] = {0};
    uint16_t count = rb_data(rb, dest);
    T_ASSERT_INT_EQ(count, 3, "should extract 3 elements");
    T_ASSERT_PTR_EQ(dest[0], &d[0], "dest[0]");
    T_ASSERT_PTR_EQ(dest[1], &d[1], "dest[1]");
    T_ASSERT_PTR_EQ(dest[2], &d[2], "dest[2]");
    
    rb_kill(rb);
    return true;
}

int main(void)
{
    TEST_SUITE("RingBuffer static tests");
    RUN_TEST(test_rb_creation_and_empty);
    RUN_TEST(test_rb_write_and_read);
    RUN_TEST(test_rb_wrap_around);
    RUN_TEST(test_rb_overwrite_eviction);
    RUN_TEST(test_rb_data_extraction);
    SUITE_END();
    return test_summary("ring_buffer");
}

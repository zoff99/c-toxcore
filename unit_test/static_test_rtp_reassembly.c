/*
 * static_test_rtp_reassembly.c
 *
 * Comprehensive unit tests for RTP video packet reassembly in toxav.
 * Tests the fix for heap buffer overflow in fill_data_into_slot and
 * exercises edge cases in get_slot, process_frame, and handle_video_packet.
 *
 * Uses the #define static trick to access internal functions.
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define static
#define inline
#include "../amalgamation/toxcore_amalgamation.c"
#undef inline
#undef static

/* AV_INPUT_BUFFER_PADDING_SIZE is defined in libavcodec */
#ifndef AV_INPUT_BUFFER_PADDING_SIZE
#define AV_INPUT_BUFFER_PADDING_SIZE 64
#endif

/* ── Helper: Create a minimal RTPSession for testing ─────────── */
static RTPSession *create_test_rtp_session(void)
{
    RTPSession *session = (RTPSession *)calloc(1, sizeof(RTPSession));
    if (!session) return NULL;
    
    session->work_buffer_list = (struct RTPWorkBufferList *)calloc(1, sizeof(struct RTPWorkBufferList));
    if (!session->work_buffer_list) {
        free(session);
        return NULL;
    }
    
    session->work_buffer_list->next_free_entry = 0;
    session->payload_type = RTP_TYPE_VIDEO;
    session->rtp_receive_active = true;
    
    return session;
}

static void destroy_test_rtp_session(RTPSession *session)
{
    if (!session) return;
    
    if (session->work_buffer_list) {
        for (int8_t i = 0; i < session->work_buffer_list->next_free_entry; ++i) {
            if (session->work_buffer_list->work_buffer[i].buf) {
                free(session->work_buffer_list->work_buffer[i].buf);
            }
        }
        free(session->work_buffer_list);
    }
    
    free(session);
}

/* Stub callback that just records calls */
static int g_callback_count = 0;
static int stub_mcb(Mono_Time *mono_time, void *cs, struct RTPMessage *msg)
{
    (void)mono_time;
    (void)cs;
    if (msg) {
        g_callback_count++;
        free(msg); /* Take ownership and free */
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: fill_data_into_slot
 * ═══════════════════════════════════════════════════════════════════ */

bool test_fill_basic_single_packet(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 100;
    header.timestamp = 12345;
    header.data_length_full = 1000;
    header.offset_full = 0;
    
    uint8_t data[1000];
    memset(data, 0xAA, sizeof(data));
    
    bool result = fill_data_into_slot(
        session->tox,
        session->work_buffer_list,
        0, /* slot_id */
        false, /* is_keyframe */
        &header,
        data,
        1000
    );
    
    T_ASSERT_TRUE(result, "single-packet frame should complete");
    T_ASSERT_INT_EQ(session->work_buffer_list->next_free_entry, 1, "one slot used");
    T_ASSERT_PTR_NOT_NULL(session->work_buffer_list->work_buffer[0].buf, "slot has buffer");
    T_ASSERT_INT_EQ(session->work_buffer_list->work_buffer[0].received_len, 1000, "received full length");
    
    /* Verify data was copied correctly */
    T_ASSERT_INT_EQ(memcmp(session->work_buffer_list->work_buffer[0].buf->data, data, 1000), 0,
                    "data should match");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_multipart_in_order(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 200;
    header.timestamp = 54321;
    header.data_length_full = 3000;
    
    uint8_t chunk[1000];
    memset(chunk, 0xBB, sizeof(chunk));
    
    /* First chunk: offset 0 */
    header.offset_full = 0;
    bool result1 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, chunk, 1000
    );
    T_ASSERT_FALSE(result1, "first chunk should not complete frame");
    
    /* Second chunk: offset 1000 */
    header.offset_full = 1000;
    bool result2 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, chunk, 1000
    );
    T_ASSERT_FALSE(result2, "second chunk should not complete frame");
    
    /* Third chunk: offset 2000 */
    header.offset_full = 2000;
    bool result3 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, chunk, 1000
    );
    T_ASSERT_TRUE(result3, "third chunk should complete frame");
    
    T_ASSERT_INT_EQ(session->work_buffer_list->work_buffer[0].received_len, 3000,
                    "should have received all 3000 bytes");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_multipart_out_of_order(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 300;
    header.timestamp = 99999;
    header.data_length_full = 3000;
    
    uint8_t chunk1[1000], chunk2[1000], chunk3[1000];
    memset(chunk1, 0x11, sizeof(chunk1));
    memset(chunk2, 0x22, sizeof(chunk2));
    memset(chunk3, 0x33, sizeof(chunk3));
    
    /* Arrive out of order: chunk3, chunk1, chunk2 */
    header.offset_full = 2000;
    fill_data_into_slot(session->tox, session->work_buffer_list, 0, false, &header, chunk3, 1000);
    
    header.offset_full = 0;
    fill_data_into_slot(session->tox, session->work_buffer_list, 0, false, &header, chunk1, 1000);
    
    header.offset_full = 1000;
    bool result = fill_data_into_slot(session->tox, session->work_buffer_list, 0, false, &header, chunk2, 1000);
    
    T_ASSERT_TRUE(result, "frame should complete when all chunks arrive");
    
    /* Verify correct placement */
    uint8_t *buf = session->work_buffer_list->work_buffer[0].buf->data;
    T_ASSERT_INT_EQ(buf[0], 0x11, "first chunk at offset 0");
    T_ASSERT_INT_EQ(buf[1000], 0x22, "second chunk at offset 1000");
    T_ASSERT_INT_EQ(buf[2000], 0x33, "third chunk at offset 2000");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_malicious_length_too_large(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 500;
    header.timestamp = 22222;
    header.data_length_full = MAX_RTP_FRAME_SIZE + 1; /* Exceeds limit */
    header.offset_full = 0;
    
    uint8_t data[100];
    
    bool result = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data, 100
    );
    
    T_ASSERT_FALSE(result, "frame exceeding MAX_RTP_FRAME_SIZE should be rejected");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_duplicate_packet(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 700;
    header.timestamp = 44444;
    header.data_length_full = 1000;
    header.offset_full = 0;
    
    uint8_t data[1000];
    memset(data, 0xFF, sizeof(data));
    
    /* First packet */
    bool result1 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data, 1000
    );
    T_ASSERT_TRUE(result1, "first packet should complete frame");
    
    /* Duplicate packet (retransmission) */
    bool result2 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data, 1000
    );
    
    /* Should fail because data_length_full mismatch (slot already has different length) */
    T_ASSERT_FALSE(result2, "duplicate packet should be rejected");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_malicious_offset_overflow(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 400;
    header.timestamp = 11111;
    header.data_length_full = 1000;
    header.offset_full = 900; /* Valid offset, but leaves only 100 bytes of space */
    
    uint8_t data[200];
    memset(data, 0xCC, sizeof(data));
    
    /* Bounds check: data_length_full - offset_full < incoming_data_length
     * 1000 - 900 < 200 → 100 < 200 → TRUE → rejected
     * This tests the bounds check WITHOUT triggering the offset < length assertion. */
    
    bool result = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data, 200
    );
    
    T_ASSERT_FALSE(result, "packet exceeding remaining space should be rejected");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_overlapping_chunks(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 600;
    header.timestamp = 33333;
    header.data_length_full = 1000;
    
    uint8_t chunk1[600];
    uint8_t chunk2[600];
    memset(chunk1, 0xDD, sizeof(chunk1));
    memset(chunk2, 0xEE, sizeof(chunk2));
    
    /* First chunk: offset 0, length 600 */
    header.offset_full = 0;
    fill_data_into_slot(session->tox, session->work_buffer_list, 0, false, &header, chunk1, 600);
    
    /* Second chunk: offset 400, length 600 (overlaps with first!)
     * Bounds check: 1000 - 400 < 600 → 600 < 600 is FALSE → passes
     * But received_len becomes 1200, which != 1000, so frame never completes.
     * This is expected behavior: overlapping retransmissions cause overshoot. */
    header.offset_full = 400;
    bool result = fill_data_into_slot(session->tox, session->work_buffer_list, 0, false, &header, chunk2, 600);
    
    /* Frame does NOT complete because received_len (1200) != data_length_full (1000) */
    T_ASSERT_FALSE(result, "overlapping chunks cause received_len overshoot, frame incomplete");
    T_ASSERT_INT_EQ(session->work_buffer_list->work_buffer[0].received_len, 1200,
                    "received_len counts all bytes including overlaps");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_fill_offset_equals_length(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 800;
    header.timestamp = 55555;
    header.data_length_full = 1000;
    
    /* First, fill bytes 0-998 (999 bytes) */
    uint8_t data_first[999];
    memset(data_first, 0xAA, sizeof(data_first));
    header.offset_full = 0;
    bool result1 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data_first, 999
    );
    T_ASSERT_FALSE(result1, "999 of 1000 bytes should not complete frame");
    
    /* Now send the last byte at offset 999 */
    uint8_t data_last[1];
    data_last[0] = 0xAB;
    header.offset_full = 999;
    bool result2 = fill_data_into_slot(
        session->tox, session->work_buffer_list, 0, false, &header, data_last, 1
    );
    
    /* Bounds check: 1000 - 999 < 1 → 1 < 1 → FALSE → passes
     * received_len becomes 999 + 1 = 1000, which equals data_length_full */
    T_ASSERT_TRUE(result2, "last byte at offset 999 should complete 1000-byte frame");
    
    destroy_test_rtp_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: get_slot
 * ═══════════════════════════════════════════════════════════════════ */

bool test_get_slot_empty(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    struct RTPHeader header = {0};
    header.sequnum = 1;
    header.timestamp = 1000;
    
    int8_t slot = get_slot(session->tox, session->work_buffer_list, false, &header, false);
    
    T_ASSERT_INT_EQ(slot, 0, "first slot should be 0");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_get_slot_multipart_match(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    /* Manually create a slot with a frame in progress */
    struct RTPWorkBuffer *slot = &session->work_buffer_list->work_buffer[0];
    slot->buf = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 1000);
    slot->buf->header.sequnum = 900;
    slot->buf->header.timestamp = 77777;
    slot->received_len = 500;
    session->work_buffer_list->next_free_entry = 1;
    
    /* Try to find slot for second packet of same frame */
    struct RTPHeader header = {0};
    header.sequnum = 900;
    header.timestamp = 77777;
    
    int8_t found_slot = get_slot(session->tox, session->work_buffer_list, false, &header, true);
    
    T_ASSERT_INT_EQ(found_slot, 0, "should find existing slot for same frame");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_get_slot_eviction_when_full(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    /* Fill all 3 slots */
    for (int i = 0; i < USED_RTP_WORKBUFFER_COUNT; i++) {
        struct RTPWorkBuffer *slot = &session->work_buffer_list->work_buffer[i];
        slot->buf = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
        slot->buf->header.sequnum = 1000 + i;
        slot->buf->header.timestamp = 10000 + i;
        slot->received_len = 50;
    }
    session->work_buffer_list->next_free_entry = USED_RTP_WORKBUFFER_COUNT;
    
    /* Try to add a new frame */
    struct RTPHeader header = {0};
    header.sequnum = 2000;
    header.timestamp = 20000;
    
    int8_t result = get_slot(session->tox, session->work_buffer_list, false, &header, false);
    
    T_ASSERT_INT_EQ(result, GET_SLOT_RESULT_DROP_OLDEST_SLOT,
                    "should request eviction when all slots full");
    
    destroy_test_rtp_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: process_frame
 * ═══════════════════════════════════════════════════════════════════ */

bool test_process_frame_shift(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    /* Fill 3 slots */
    for (int i = 0; i < 3; i++) {
        struct RTPWorkBuffer *slot = &session->work_buffer_list->work_buffer[i];
        slot->buf = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
        slot->buf->header.sequnum = 3000 + i;
        slot->received_len = 50;
    }
    session->work_buffer_list->next_free_entry = 3;
    
    /* Process slot 0 (should shift slots 1 and 2 down) */
    struct RTPMessage *frame = process_frame(session->tox, session->work_buffer_list, 0);
    
    T_ASSERT_PTR_NOT_NULL(frame, "should return frame");
    T_ASSERT_INT_EQ(frame->header.sequnum, 3000, "should be frame from slot 0");
    T_ASSERT_INT_EQ(session->work_buffer_list->next_free_entry, 2, "should have 2 slots left");
    T_ASSERT_INT_EQ(session->work_buffer_list->work_buffer[0].buf->header.sequnum, 3001,
                    "slot 1 should shift to slot 0");
    T_ASSERT_INT_EQ(session->work_buffer_list->work_buffer[1].buf->header.sequnum, 3002,
                    "slot 2 should shift to slot 1");
    
    free(frame);
    destroy_test_rtp_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: handle_video_packet (integration)
 * ═══════════════════════════════════════════════════════════════════ */

bool test_handle_video_sanity_zero_length(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    session->mcb = stub_mcb;
    session->cs = (void *)0x12345; /* Dummy pointer */
    g_callback_count = 0;
    
    struct RTPHeader header = {0};
    header.data_length_full = 0; /* Invalid */
    header.offset_full = 0;
    
    uint8_t data[100];
    
    int result = handle_video_packet(session, &header, data, 100, NULL);
    
    T_ASSERT_INT_EQ(result, -1, "zero-length frame should be rejected");
    T_ASSERT_INT_EQ(g_callback_count, 0, "callback should not be called");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_handle_video_offset_equals_length(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    session->mcb = stub_mcb;
    session->cs = (void *)0x12345;
    g_callback_count = 0;
    
    struct RTPHeader header = {0};
    header.data_length_full = 1000;
    header.offset_full = 1000; /* Invalid: offset == length */
    
    uint8_t data[100];
    
    int result = handle_video_packet(session, &header, data, 100, NULL);
    
    T_ASSERT_INT_EQ(result, -1, "offset == length should be rejected");
    T_ASSERT_INT_EQ(g_callback_count, 0, "callback should not be called");
    
    destroy_test_rtp_session(session);
    return true;
}

bool test_handle_video_offset_greater_than_length(void)
{
    RTPSession *session = create_test_rtp_session();
    T_ASSERT_PTR_NOT_NULL(session, "alloc failed");
    
    session->mcb = stub_mcb;
    session->cs = (void *)0x12345;
    g_callback_count = 0;
    
    struct RTPHeader header = {0};
    header.data_length_full = 1000;
    header.offset_full = 2000; /* Invalid: offset > length */
    
    uint8_t data[100];
    
    int result = handle_video_packet(session, &header, data, 100, NULL);
    
    T_ASSERT_INT_EQ(result, -1, "offset > length should be rejected");
    T_ASSERT_INT_EQ(g_callback_count, 0, "callback should not be called");
    
    destroy_test_rtp_session(session);
    return true;
}

int main(void)
{
    TEST_SUITE("RTP Video Packet Reassembly Tests");
    
    RUN_TEST(test_fill_basic_single_packet);
    RUN_TEST(test_fill_multipart_in_order);
    RUN_TEST(test_fill_multipart_out_of_order);
    RUN_TEST(test_fill_malicious_offset_overflow);
    RUN_TEST(test_fill_malicious_length_too_large);
    RUN_TEST(test_fill_overlapping_chunks);
    RUN_TEST(test_fill_duplicate_packet);
    RUN_TEST(test_fill_offset_equals_length);
    
    RUN_TEST(test_get_slot_empty);
    RUN_TEST(test_get_slot_multipart_match);
    RUN_TEST(test_get_slot_eviction_when_full);
    
    RUN_TEST(test_process_frame_shift);
    
    RUN_TEST(test_handle_video_sanity_zero_length);
    RUN_TEST(test_handle_video_offset_equals_length);
    RUN_TEST(test_handle_video_offset_greater_than_length);
    
    SUITE_END();
    return test_summary("rtp_reassembly");
}

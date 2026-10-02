/*
 * static_test_video_logic.c
 *
 * Hardened unit tests for video.c using the amalgamation include trick.
 * Tests decoder switching, queue message handling, buffer eviction,
 * timestamp filtering, vc_iterate sequence number drop/reset logic,
 * and extreme edge cases including NULL inputs, wrap-around bugs,
 * and global sync flag behavior.
 *
 * ADVERSARIAL TESTS: Several tests at the end intentionally trigger
 * segmentation faults (SIGSEGV) to document missing NULL checks and
 * unsafe pointer dereferences in the original video.c code. The test
 * framework catches these crashes and reports them as FAIL, effectively
 * documenting the bugs without killing the entire test suite.
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>

/* Include the amalgamation to expose internal functions and structs */
#define static
#define inline
#include "../amalgamation/toxcore_amalgamation.c"
#undef inline
#undef static

/* ── Global setup for real Tox and Mono_Time instances ─────────── */
static Tox *test_tox = NULL;
static Mono_Time *test_mt = NULL;

static void global_setup(void) {
    if (!test_tox) {
        test_tox = tox_new(NULL, NULL);
    }
    if (!test_mt) {
        test_mt = mono_time_new(NULL, NULL);
    }
}

/* ── Helper: Create a minimal VCSession for testing ────────────── */
static VCSession *create_test_vc_session(void) {
    global_setup();
    VCSession *vc = (VCSession *)calloc(1, sizeof(VCSession));
    if (!vc) return NULL;
    
    pthread_mutex_init(vc->queue_mutex, NULL);
    vc->vbuf_raw = tsb_new(10);
    
    vc->av = (ToxAV *)calloc(1, sizeof(ToxAV));
    vc->av->tox = test_tox;
    vc->av->toxav_mono_time = test_mt;
    vc->friend_number = 1;
    vc->show_own_video = 0;
    
    return vc;
}

static void destroy_test_vc_session(VCSession *vc) {
    if (!vc) return;
    if (vc->vbuf_raw) {
        tsb_drain((TSBuffer *)vc->vbuf_raw);
        tsb_kill((TSBuffer *)vc->vbuf_raw);
    }
    if (vc->av) free(vc->av);
    pthread_mutex_destroy(vc->queue_mutex);
    free(vc);
}

/* ── Mock callback for video_switch_decoder ────────────────────── */
static int g_comm_cb_calls = 0;
static TOXAV_CALL_COMM_INFO g_last_cmi = 0;

static void mock_call_comm_cb(ToxAV *av, uint32_t friend_number, TOXAV_CALL_COMM_INFO comm_info, int64_t value, void *user_data) {
    (void)av; (void)friend_number; (void)value; (void)user_data;
    g_comm_cb_calls++;
    g_last_cmi = comm_info;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: video_switch_decoder
 * ═══════════════════════════════════════════════════════════════════ */

bool test_video_switch_decoder_vp8_to_h264(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_decoder_codec_used = TOXAV_ENCODER_CODEC_USED_VP8;
    vc->av->call_comm_cb = mock_call_comm_cb;
    g_comm_cb_calls = 0;
    
    video_switch_decoder(vc, TOXAV_ENCODER_CODEC_USED_H264);
    
    T_ASSERT_INT_EQ(vc->video_decoder_codec_used, TOXAV_ENCODER_CODEC_USED_H264, "codec updated");
    T_ASSERT_INT_EQ(g_comm_cb_calls, 1, "callback fired");
    T_ASSERT_INT_EQ(g_last_cmi, TOXAV_CALL_COMM_DECODER_IN_USE_H264, "correct CMI");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_video_switch_decoder_same(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_decoder_codec_used = TOXAV_ENCODER_CODEC_USED_H264;
    vc->av->call_comm_cb = mock_call_comm_cb;
    g_comm_cb_calls = 0;
    
    video_switch_decoder(vc, TOXAV_ENCODER_CODEC_USED_H264);
    
    T_ASSERT_INT_EQ(g_comm_cb_calls, 0, "callback NOT fired for same codec");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_video_switch_decoder_invalid(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_decoder_codec_used = TOXAV_ENCODER_CODEC_USED_VP8;
    vc->av->call_comm_cb = mock_call_comm_cb;
    g_comm_cb_calls = 0;
    
    video_switch_decoder(vc, (TOXAV_ENCODER_CODEC_USED_VALUE)99);
    
    T_ASSERT_INT_EQ(vc->video_decoder_codec_used, TOXAV_ENCODER_CODEC_USED_VP8, "codec NOT updated");
    T_ASSERT_INT_EQ(g_comm_cb_calls, 0, "callback NOT fired for invalid codec");
    
    destroy_test_vc_session(vc);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: vc_queue_message (basic)
 * ═══════════════════════════════════════════════════════════════════ */

bool test_vc_queue_message_invalid_pt(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_AUDIO % 128;
    
    int res = vc_queue_message(test_mt, vc, msg);
    
    T_ASSERT_INT_EQ(res, -1, "should return -1 for invalid PT");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_dummy_pt(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = (RTP_TYPE_VIDEO + 2) % 128;
    
    int res = vc_queue_message(test_mt, vc, msg);
    
    T_ASSERT_INT_EQ(res, 0, "should return 0 for dummy PT");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_timestamp_fallback(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.timestamp = 55555;
    msg->header.frame_record_timestamp = 0;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->header.data_length_full = 50;
    msg->len = 50;
    
    int res = vc_queue_message(test_mt, vc, msg);
    T_ASSERT_INT_EQ(res, 0, "should succeed");
    
    uint32_t min_ts = 0, max_ts = 0;
    tsb_get_range_in_buffer(test_tox, (TSBuffer *)vc->vbuf_raw, &min_ts, &max_ts);
    
    T_ASSERT_INT_EQ(max_ts, 55555, "frame_record_timestamp should fallback to timestamp");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_gap_calculation(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg1 = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg1->header.pt = RTP_TYPE_VIDEO % 128;
    msg1->header.flags = RTP_LARGE_FRAME;
    msg1->header.data_length_full = 50;
    msg1->len = 50;
    msg1->header.frame_record_timestamp = 1000;
    
    vc_queue_message(test_mt, vc, msg1);
    T_ASSERT_INT_EQ(vc->incoming_video_frames_gap_ms_mean_value, 0, "mean should be 0 after first frame");
    
    vc->incoming_video_frames_gap_last_ts = current_time_monotonic(test_mt) - 33;
    
    struct RTPMessage *msg2 = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg2->header.pt = RTP_TYPE_VIDEO % 128;
    msg2->header.flags = RTP_LARGE_FRAME;
    msg2->header.data_length_full = 50;
    msg2->len = 50;
    msg2->header.frame_record_timestamp = 2000;
    
    vc_queue_message(test_mt, vc, msg2);
    
    T_ASSERT_INT_GT(vc->incoming_video_frames_gap_ms_mean_value, 0, "mean should be > 0 after second frame with gap");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_show_own_video_discards(void) {
    VCSession *vc = create_test_vc_session();
    vc->show_own_video = 1;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->header.frame_record_timestamp = 1000;
    msg->len = 50;
    
    int res = vc_queue_message(test_mt, vc, msg);
    
    T_ASSERT_INT_EQ(res, 0, "should succeed");
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 0, "buffer should remain empty when show_own_video is 1");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_buffer_eviction(void) {
    VCSession *vc = create_test_vc_session();
    
    for (int i = 0; i < 10; i++) {
        struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 50);
        msg->header.pt = RTP_TYPE_VIDEO % 128;
        msg->header.flags = RTP_LARGE_FRAME;
        msg->header.frame_record_timestamp = 1000 + i;
        msg->len = 20;
        vc_queue_message(test_mt, vc, msg);
    }
    
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 10, "buffer full");
    
    struct RTPMessage *msg11 = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 50);
    msg11->header.pt = RTP_TYPE_VIDEO % 128;
    msg11->header.flags = RTP_LARGE_FRAME;
    msg11->header.frame_record_timestamp = 2000;
    msg11->len = 20;
    vc_queue_message(test_mt, vc, msg11);
    
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 10, "buffer size remains 10 after eviction");
    
    uint32_t min_ts = 0, max_ts = 0;
    tsb_get_range_in_buffer(test_tox, (TSBuffer *)vc->vbuf_raw, &min_ts, &max_ts);
    
    T_ASSERT_INT_EQ(min_ts, 1001, "oldest frame (1000) should have been evicted");
    T_ASSERT_INT_EQ(max_ts, 2000, "newest frame (2000) should be present");
    
    destroy_test_vc_session(vc);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: vc_iterate (basic)
 * ═══════════════════════════════════════════════════════════════════ */

bool test_vc_iterate_null_vc(void) {
    uint8_t res = vc_iterate(NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    T_ASSERT_INT_EQ(res, 0, "should return 0 for NULL vc");
    return true;
}

bool test_vc_iterate_empty_queue(void) {
    VCSession *vc = create_test_vc_session();
    
    uint64_t a_r = 0, a_l = 0, v_r = 0, v_l = 0;
    int64_t adj_audio = 0, diff_sender = 0;
    int32_t has_rtt = 0;
    
    uint8_t res = vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj_audio, &diff_sender, &has_rtt);
    
    T_ASSERT_INT_EQ(res, 0, "should return 0 when jbuf is empty");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_drops_old_seqnum(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    vc->last_seen_fragment_seqnum = 100;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 50;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    
    uint8_t res = vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    T_ASSERT_INT_EQ(res, 0, "should return 0 for dropped frame");
    T_ASSERT_INT_EQ(vc->count_old_video_frames_seen, 1, "old frame counter incremented");
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 0, "buffer should be empty after drop");
    T_ASSERT_INT_EQ(vc->last_seen_fragment_seqnum, 100, "seqnum should NOT reset yet");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_old_seqnum_reset(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    vc->last_seen_fragment_seqnum = 100;
    
    for (int i = 0; i < 7; i++) {
        struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
        msg->header.pt = RTP_TYPE_VIDEO % 128;
        msg->header.sequnum = 50;
        msg->header.frame_record_timestamp = 1000 + i;
        msg->header.flags = RTP_LARGE_FRAME;
        msg->len = 50;
        vc_queue_message(test_mt, vc, msg);
        
        uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
        int64_t adj=0, diff=0;
        int32_t has_rtt=0;
        vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    }
    
    T_ASSERT_INT_EQ(vc->count_old_video_frames_seen, 0, "counter reset after >6 old frames");
    T_ASSERT_INT_EQ(vc->last_seen_fragment_seqnum, 50, "seqnum baseline reset to 50");
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 0, "buffer empty");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_out_of_range_timestamp(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 1;
    vc->has_rountrip_time_ms = 1;
    vc->encoder_frame_has_record_timestamp = 1;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 50);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->header.frame_record_timestamp = 5000;
    msg->len = 20;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    
    uint8_t res = vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    T_ASSERT_INT_EQ(res, 0, "should return 0 when no frame in timestamp range");
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 1, "frame should remain in buffer");
    
    destroy_test_vc_session(vc);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: EXTREME EDGE CASES & SECURITY
 * ═══════════════════════════════════════════════════════════════════ */

bool test_vc_queue_message_null_vc(void) {
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    int res = vc_queue_message(test_mt, NULL, msg);
    T_ASSERT_INT_EQ(res, -1, "should return -1 for NULL vc");
    return true;
}

bool test_vc_queue_message_null_msg(void) {
    VCSession *vc = create_test_vc_session();
    int res = vc_queue_message(test_mt, vc, NULL);
    T_ASSERT_INT_EQ(res, -1, "should return -1 for NULL msg");
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_seqnum_wraparound_bug(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    vc->last_seen_fragment_seqnum = UINT16_MAX;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 0;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    T_ASSERT_INT_EQ(vc->count_old_video_frames_seen, 1, "wrapped seqnum 0 treated as old (known bug)");
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 0, "frame dropped");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_non_large_frame_path(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.flags = 0;
    msg->header.frame_record_timestamp = 9999;
    msg->len = 50;
    
    int res = vc_queue_message(test_mt, vc, msg);
    T_ASSERT_INT_EQ(res, 0, "should succeed");
    
    T_ASSERT_INT_EQ(tsb_size((TSBuffer *)vc->vbuf_raw), 1, "frame queued via non-large-frame path");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_global_do_not_sync_av(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 1;
    vc->has_rountrip_time_ms = 1;
    vc->last_seen_fragment_seqnum = 100;
    
    extern bool global_do_not_sync_av;
    bool old_val = global_do_not_sync_av;
    global_do_not_sync_av = true;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 50;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    uint8_t res = vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    global_do_not_sync_av = old_val;
    
    T_ASSERT_INT_EQ(res, 0, "frame read and dropped due to old seqnum");
    T_ASSERT_INT_EQ(vc->count_old_video_frames_seen, 1, "proves frame was read despite timestamps");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_max_timestamp(void) {
    VCSession *vc = create_test_vc_session();
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->header.frame_record_timestamp = UINT32_MAX;
    msg->len = 50;
    
    int res = vc_queue_message(test_mt, vc, msg);
    T_ASSERT_INT_EQ(res, 0, "should handle UINT32_MAX timestamp");
    
    uint32_t min_ts = 0, max_ts = 0;
    tsb_get_range_in_buffer(test_tox, (TSBuffer *)vc->vbuf_raw, &min_ts, &max_ts);
    T_ASSERT_INT_EQ(max_ts, UINT32_MAX, "timestamp stored correctly");
    
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_first_frame_flag_transition(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    vc->last_seen_fragment_seqnum = 100;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 50;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    T_ASSERT_INT_EQ(vc->video_received_first_frame, 1, "first frame flag should be set after reading a frame");
    
    destroy_test_vc_session(vc);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * ADVERSARIAL TESTS: Documenting crashes (SIGSEGV) in video.c
 * These tests intentionally trigger NULL pointer dereferences to
 * document missing safety checks in the original code.
 * ═══════════════════════════════════════════════════════════════════ */

bool test_vc_iterate_null_out_params_crash(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 50;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    /* BUG: video.c does not check if out-parameters are NULL.
     * It immediately does: *timestamp_difference_to_sender_ = ...
     * This will segfault, documenting the missing NULL check. */
    uint8_t res = vc_iterate(vc, test_tox, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    
    T_ASSERT_INT_EQ(res, 0, "should handle NULL out params gracefully (but crashes)");
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_iterate_null_bwc_crash(void) {
    VCSession *vc = create_test_vc_session();
    vc->video_received_first_frame = 0;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_VIDEO % 128;
    msg->header.sequnum = 50;
    msg->header.frame_record_timestamp = 1000;
    msg->header.flags = RTP_LARGE_FRAME;
    msg->header.data_length_full = 50;
    msg->len = 50;
    vc_queue_message(test_mt, vc, msg);
    
    uint64_t a_r=0, a_l=0, v_r=0, v_l=0;
    int64_t adj=0, diff=0;
    int32_t has_rtt=0;
    
    /* BUG: video.c calls bwc_add_recv(bwc, ...) without checking if bwc is NULL.
     * This will segfault inside bwc_add_recv, documenting the missing NULL check. */
    uint8_t res = vc_iterate(vc, test_tox, 0, &a_r, &a_l, &v_r, &v_l, NULL, &adj, &diff, &has_rtt);
    
    T_ASSERT_INT_EQ(res, 0, "should handle NULL bwc gracefully (but crashes)");
    destroy_test_vc_session(vc);
    return true;
}

bool test_vc_queue_message_null_av_crash(void) {
    VCSession *vc = create_test_vc_session();
    free(vc->av);
    vc->av = NULL;
    
    struct RTPMessage *msg = (struct RTPMessage *)calloc(1, sizeof(struct RTPMessage) + 100);
    msg->header.pt = RTP_TYPE_AUDIO % 128;
    
    /* BUG: video.c does: LOGGER_API_WARNING(vc->av->tox, "Invalid payload type! ...")
     * If vc->av is NULL, vc->av->tox dereferences NULL and segfaults. */
    int res = vc_queue_message(test_mt, vc, msg);
    
    T_ASSERT_INT_EQ(res, -1, "should handle NULL av gracefully (but crashes)");
    destroy_test_vc_session(vc);
    return true;
}

int main(void) {
    TEST_SUITE("Video Session Logic Tests (Amalgamation)");
    
    /* Basic functionality */
    RUN_TEST(test_video_switch_decoder_vp8_to_h264);
    RUN_TEST(test_video_switch_decoder_same);
    RUN_TEST(test_video_switch_decoder_invalid);
    
    RUN_TEST(test_vc_queue_message_invalid_pt);
    RUN_TEST(test_vc_queue_message_dummy_pt);
    RUN_TEST(test_vc_queue_message_timestamp_fallback);
    RUN_TEST(test_vc_queue_message_gap_calculation);
    RUN_TEST(test_vc_queue_message_show_own_video_discards);
    RUN_TEST(test_vc_queue_message_buffer_eviction);
    
    RUN_TEST(test_vc_iterate_null_vc);
    RUN_TEST(test_vc_iterate_empty_queue);
    RUN_TEST(test_vc_iterate_drops_old_seqnum);
    RUN_TEST(test_vc_iterate_old_seqnum_reset);
    RUN_TEST(test_vc_iterate_out_of_range_timestamp);
    
    /* Extreme edge cases & security */
    RUN_TEST(test_vc_queue_message_null_vc);
    RUN_TEST(test_vc_queue_message_null_msg);
    RUN_TEST(test_vc_iterate_seqnum_wraparound_bug);
    RUN_TEST(test_vc_queue_message_non_large_frame_path);
    RUN_TEST(test_vc_iterate_global_do_not_sync_av);
    RUN_TEST(test_vc_queue_message_max_timestamp);
    RUN_TEST(test_vc_iterate_first_frame_flag_transition);
    
    /* Adversarial tests (expect SIGSEGV failures documenting bugs) */
    RUN_TEST(test_vc_iterate_null_out_params_crash);
    RUN_TEST(test_vc_iterate_null_bwc_crash);
    RUN_TEST(test_vc_queue_message_null_av_crash);
    
    SUITE_END();
    
    if (test_mt) mono_time_free(test_mt);
    if (test_tox) tox_kill(test_tox);
    
    return test_summary("video_logic_amalgamation");
}

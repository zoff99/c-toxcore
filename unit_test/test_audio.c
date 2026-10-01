/*
 * test_audio.c
 *
 * unit tests for audio.c logic.
 * 
 * Since audio.c is heavily coupled with Opus, ToxAV, and toxcore internals,
 * we extract and mock the core sequence number handling and payload validation
 * logic to verify it in isolation without requiring the entire toxav build tree.
 */

#include "test_framework.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

/* ── MOCKS & STUBS ─────────────────────────────────────────────── */

#define RTP_TYPE_AUDIO 192
#define RTP_TYPE_VIDEO 193
#define AUDIO_LOST_FRAME_INDICATOR 2

struct RTPHeader {
    uint8_t pt;
    uint16_t sequnum;
    uint32_t timestamp;
    uint32_t frame_record_timestamp;
};

struct RTPMessage {
    struct RTPHeader header;
    uint8_t *data;
    uint32_t len;
};

/* Mock ACSession containing only the fields required for sequence logic */
typedef struct MockACSession {
    int32_t lp_seqnum_new;
} MockACSession;

/* Extracted sequence number logic from jbuf_read */
static int process_sequence_number(MockACSession *ac, uint16_t sequnum, bool *lost_frame)
{
    *lost_frame = false;
    
    if (ac->lp_seqnum_new == -1) {
        ac->lp_seqnum_new = sequnum;
        return 1; /* success, no drop */
    }

    if (
        (
            ((sequnum > 5) && (ac->lp_seqnum_new < (UINT16_MAX - 5)))
        ) &&
        (sequnum <= ac->lp_seqnum_new)
    ) {
        return 0; /* drop packet */
    }

    if (
        ((sequnum > 8) && (ac->lp_seqnum_new < (UINT16_MAX - 7)))
    ) {
        int64_t diff = (sequnum - ac->lp_seqnum_new);
        if (diff > 1) {
            *lost_frame = true;
        }
    }

    ac->lp_seqnum_new = sequnum;
    return 1; /* accepted */
}

/* Extracted payload type validation from ac_queue_message */
static int validate_payload_type(uint8_t pt)
{
    if ((pt & 0x7f) == (RTP_TYPE_AUDIO + 2) % 128) {
        return 0; /* dummy packet */
    }
    if ((pt & 0x7f) != RTP_TYPE_AUDIO % 128) {
        return -1; /* invalid */
    }
    return 1; /* valid audio */
}

/* ── TESTS ─────────────────────────────────────────────────────── */

bool test_audio_seq_first_packet(void)
{
    MockACSession ac = { .lp_seqnum_new = -1 };
    bool lost = false;
    
    int res = process_sequence_number(&ac, 100, &lost);
    T_ASSERT_INT_EQ(res, 1, "first packet accepted");
    T_ASSERT_FALSE(lost, "no lost frames on first packet");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 100, "seqnum updated");
    
    return true;
}

bool test_audio_seq_normal_increment(void)
{
    MockACSession ac = { .lp_seqnum_new = 100 };
    bool lost = false;
    
    int res = process_sequence_number(&ac, 101, &lost);
    T_ASSERT_INT_EQ(res, 1, "next packet accepted");
    T_ASSERT_FALSE(lost, "no lost frames");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 101, "seqnum updated");
    
    return true;
}

bool test_audio_seq_missing_frame(void)
{
    MockACSession ac = { .lp_seqnum_new = 100 };
    bool lost = false;
    
    /* Skip 101, receive 102 */
    int res = process_sequence_number(&ac, 102, &lost);
    T_ASSERT_INT_EQ(res, 1, "packet accepted");
    T_ASSERT_TRUE(lost, "should detect missing frame");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 102, "seqnum updated");
    
    return true;
}

bool test_audio_seq_missing_multiple_frames(void)
{
    MockACSession ac = { .lp_seqnum_new = 100 };
    bool lost = false;
    
    /* Skip 101, 102, 103, receive 104 */
    int res = process_sequence_number(&ac, 104, &lost);
    T_ASSERT_INT_EQ(res, 1, "packet accepted");
    T_ASSERT_TRUE(lost, "should detect missing frames");
    
    return true;
}

bool test_audio_seq_drop_old_packet(void)
{
    MockACSession ac = { .lp_seqnum_new = 100 };
    bool lost = false;
    
    /* Receive 99 (older than 100) */
    int res = process_sequence_number(&ac, 99, &lost);
    T_ASSERT_INT_EQ(res, 0, "old packet should be dropped");
    T_ASSERT_FALSE(lost, "no lost frame indicator for dropped old packet");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 100, "seqnum should NOT be updated");
    
    return true;
}

bool test_audio_seq_drop_duplicate(void)
{
    MockACSession ac = { .lp_seqnum_new = 100 };
    bool lost = false;
    
    /* Receive 100 again */
    int res = process_sequence_number(&ac, 100, &lost);
    T_ASSERT_INT_EQ(res, 0, "duplicate packet should be dropped");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 100, "seqnum unchanged");
    
    return true;
}

bool test_audio_seq_wraparound_forward(void)
{
    MockACSession ac = { .lp_seqnum_new = UINT16_MAX - 2 };
    bool lost = false;
    
    /* Wrap around to 0, 1, 2 */
    process_sequence_number(&ac, UINT16_MAX - 1, &lost);
    process_sequence_number(&ac, UINT16_MAX, &lost);
    
    int res = process_sequence_number(&ac, 0, &lost);
    T_ASSERT_INT_EQ(res, 1, "wrapped packet accepted");
    T_ASSERT_FALSE(lost, "no lost frames on wrap");
    T_ASSERT_INT_EQ(ac.lp_seqnum_new, 0, "seqnum wrapped to 0");
    
    return true;
}

bool test_audio_seq_wraparound_drop_old(void)
{
    MockACSession ac = { .lp_seqnum_new = 5 }; /* Just wrapped */
    bool lost = false;
    
    /* Receive UINT16_MAX (old packet from before wrap).
     * Trace: sequnum (65535) > 5 (True). ac->lp_seqnum_new (5) < 65530 (True).
     * sequnum (65535) <= ac->lp_seqnum_new (5) -> False.
     * So it is ACCEPTED and treated as a huge jump! 
     * This documents a known edge case in the sequence number logic where
     * late packets arriving after a wraparound are misinterpreted as massive jumps. */
    int res = process_sequence_number(&ac, UINT16_MAX, &lost);
    T_ASSERT_INT_EQ(res, 1, "documents edge case: late packet after wrap is accepted as jump");
    T_ASSERT_TRUE(lost, "documents edge case: triggers lost frame due to huge diff");
    
    return true;
}

bool test_audio_payload_valid(void)
{
    int res = validate_payload_type(RTP_TYPE_AUDIO);
    T_ASSERT_INT_EQ(res, 1, "valid audio payload");
    
    /* With marker bit set (0x80) */
    res = validate_payload_type(RTP_TYPE_AUDIO | 0x80);
    T_ASSERT_INT_EQ(res, 1, "valid audio payload with marker bit");
    
    return true;
}

bool test_audio_payload_dummy(void)
{
    int res = validate_payload_type((RTP_TYPE_AUDIO + 2) % 128);
    T_ASSERT_INT_EQ(res, 0, "dummy packet");
    
    return true;
}

bool test_audio_payload_invalid(void)
{
    int res = validate_payload_type(RTP_TYPE_VIDEO);
    T_ASSERT_INT_EQ(res, -1, "invalid payload (video)");
    
    res = validate_payload_type(0);
    T_ASSERT_INT_EQ(res, -1, "invalid payload (0)");
    
    return true;
}

int main(void)
{
    TEST_SUITE("Audio session logic tests (Mocked)");
    
    /* Sequence number handling */
    RUN_TEST(test_audio_seq_first_packet);
    RUN_TEST(test_audio_seq_normal_increment);
    RUN_TEST(test_audio_seq_missing_frame);
    RUN_TEST(test_audio_seq_missing_multiple_frames);
    RUN_TEST(test_audio_seq_drop_old_packet);
    RUN_TEST(test_audio_seq_drop_duplicate);
    RUN_TEST(test_audio_seq_wraparound_forward);
    RUN_TEST(test_audio_seq_wraparound_drop_old);
    
    /* Payload type validation */
    RUN_TEST(test_audio_payload_valid);
    RUN_TEST(test_audio_payload_dummy);
    RUN_TEST(test_audio_payload_invalid);
    
    SUITE_END();
    return test_summary("audio_session");
}

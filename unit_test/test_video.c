/*
 * test_video.c
 *
 * Static unit tests for video.c logic.
 * 
 * NOTE: video.c is heavily coupled with ToxAV, VPX/H264 codecs, and toxcore internals.
 * To statically unit test it without pulling in the entire toxav build tree, we extract 
 * and mock the complex frame-gap calculation logic to verify it in isolation.
 */

#include "test_framework.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

/* --- MOCKS & STUBS --- */
#define VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES 10

/* Stub VCSession containing only the fields required for the gap calculation logic */
typedef struct VCSession {
    uint32_t incoming_video_frames_gap_last_ts;
    uint32_t incoming_video_frames_gap_ms[VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES];
    uint8_t incoming_video_frames_gap_ms_index;
    uint32_t incoming_video_frames_gap_ms_mean_value;
} VCSession;

/* Extracted logic from vc_queue_message for isolated testing */
static void calculate_incoming_gap(VCSession *vc, uint64_t current_time)
{
    if (vc->incoming_video_frames_gap_last_ts > 0) {
        uint32_t curent_gap = current_time - vc->incoming_video_frames_gap_last_ts;

        vc->incoming_video_frames_gap_ms[vc->incoming_video_frames_gap_ms_index] = curent_gap;
        vc->incoming_video_frames_gap_ms_index = (vc->incoming_video_frames_gap_ms_index + 1) %
                VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES;

        uint32_t mean_value = 0;
        for (int k = 0; k < VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES; k++) {
            mean_value = mean_value + vc->incoming_video_frames_gap_ms[k];
        }

        if (mean_value == 0) {
            vc->incoming_video_frames_gap_ms_mean_value = 0;
        } else {
            vc->incoming_video_frames_gap_ms_mean_value = (mean_value * 10) / (VIDEO_INCOMING_FRAMES_GAP_MS_ENTRIES * 10);
        }
    }
    vc->incoming_video_frames_gap_last_ts = current_time;
}

bool test_video_gap_calculation_first_frame(void)
{
    VCSession vc = {0};
    
    /* First frame: last_ts is 0, should not calculate gap, just set last_ts */
    calculate_incoming_gap(&vc, 1000);
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_last_ts, 1000, "last_ts updated");
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms_mean_value, 0, "mean remains 0");
    
    return true;
}

bool test_video_gap_calculation_subsequent_frames(void)
{
    VCSession vc = {0};
    
    /* Frame 1 */
    calculate_incoming_gap(&vc, 1000);
    /* Frame 2 (33ms later -> ~30fps) */
    calculate_incoming_gap(&vc, 1033);
    
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms[0], 33, "gap recorded");
    /* mean = (33 * 10) / 100 = 3 */
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms_mean_value, 3, "mean calculated");
    
    /* Frame 3 (33ms later) */
    calculate_incoming_gap(&vc, 1066);
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms[1], 33, "second gap recorded");
    /* mean = (33 + 33) * 10 / 100 = 6 */
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms_mean_value, 6, "mean updated");
    
    return true;
}

bool test_video_gap_wrap_around(void)
{
    VCSession vc = {0};
    
    /* Fill the 10-element ring buffer for gaps */
    for (int i = 0; i < 12; i++) {
        calculate_incoming_gap(&vc, 1000 + (i * 10));
    }
    
    /* The index should have wrapped around (11 % 10 = 1) */
    T_ASSERT_INT_EQ(vc.incoming_video_frames_gap_ms_index, 1, "index wrapped");
    
    return true;
}

int main(void)
{
    TEST_SUITE("Video session logic tests (Mocked)");
    RUN_TEST(test_video_gap_calculation_first_frame);
    RUN_TEST(test_video_gap_calculation_subsequent_frames);
    RUN_TEST(test_video_gap_wrap_around);
    SUITE_END();
    return test_summary("video_session");
}

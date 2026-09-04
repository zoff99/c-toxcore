#include "test_framework.h"
#include "../../toxcore/tox.h"
#include "../mid_roster.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

extern Tox *create_dummy_tox(void);
extern void mock_set_churn(int on);

static const uint8_t chat_id_1[TOX_GROUP_CHAT_ID_SIZE] = {1};
static const uint8_t chat_id_2[TOX_GROUP_CHAT_ID_SIZE] = {2};
static const uint8_t chat_id_3[TOX_GROUP_CHAT_ID_SIZE] = {3};
static const uint8_t chat_id_4[TOX_GROUP_CHAT_ID_SIZE] = {4};

/*
 * Test that the middleware enforces the MID_MAX_PEERS_PER_GROUP limit
 * across multiple groups, and gracefully drops peers beyond the limit
 * without crashing, leaking, or corrupting memory.
 */
static bool test_peer_limit_multiple_groups(void)
{
    /* Ensure each peer_id gets a distinct identity key so the roster actually grows */
    mock_set_churn(1);

    MidState *s = mid_new(NULL, NULL, 0);
    T_ASSERT_PTR_NOT_NULL(s, "mid_new()");

    Tox *tox = create_dummy_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "create_dummy_tox()");

    const uint8_t *chat_ids[4] = {chat_id_1, chat_id_2, chat_id_3, chat_id_4};

    /* Create 3 groups */
    mid_on_group_self_join(s, tox, 1, (const uint8_t *)"g1", 2);
    mid_on_group_self_join(s, tox, 2, (const uint8_t *)"g2", 2);
    mid_on_group_self_join(s, tox, 3, (const uint8_t *)"g3", 2);

    T_ASSERT_INT_EQ(mid_group_count(s), 3, "3 groups created");

    /* 
     * The limit is defined as MID_MAX_PEERS_PER_GROUP.
     * We will attempt to add 8500 peers to each group.
     * The self peer is already in the group, so the roster can accept 4095 more.
     */
    size_t limit = MID_MAX_PEERS_PER_GROUP;
    uint32_t target_peers = 8500;

    for (int g = 0; g < 3; g++) {
        for (uint32_t i = 1; i <= target_peers; i++) {
            /* Make peer_id unique per group */
            uint32_t peer_id = ((g + 1) * 100000) + i;
            mid_on_group_peer_join(s, tox, g + 1, peer_id);
        }
        
        /* The count should be exactly the limit. */
        size_t count = mid_peer_count(s, chat_ids[g]);
        T_ASSERT_INT_EQ(count, limit, "peer count capped at limit");
    }

    T_ASSERT_INT_EQ(mid_group_count(s), 3, "still 3 groups");

    /* Verify counts are stable */
    T_ASSERT_INT_EQ(mid_peer_count(s, chat_ids[0]), limit, "group 1 count stable");
    T_ASSERT_INT_EQ(mid_peer_count(s, chat_ids[1]), limit, "group 2 count stable");
    T_ASSERT_INT_EQ(mid_peer_count(s, chat_ids[2]), limit, "group 3 count stable");

    /* Verify we can still add a 4th group and it also respects the limit */
    mid_on_group_self_join(s, tox, 4, (const uint8_t *)"g4", 2);
    T_ASSERT_INT_EQ(mid_group_count(s), 4, "4th group added successfully");

    for (uint32_t i = 1; i <= target_peers; i++) {
        mid_on_group_peer_join(s, tox, 4, 400000 + i);
    }
    T_ASSERT_INT_EQ(mid_peer_count(s, chat_ids[3]), limit, "group 4 peer count capped at limit");

    mid_free(s);
    mock_set_churn(0);
    return true;
}

int main(void)
{
    TEST_SUITE("midroster limits / stress tests");

    RUN_TEST(test_peer_limit_multiple_groups);

    SUITE_END();

    return test_summary("midroster_limits");
}

#include "test_framework.h"
#include "../../toxcore/tox.h"
#include "../mid_roster.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

extern Tox *create_dummy_tox(void);
extern void mock_set_churn(int on);

/*
How many peer joins to attempt per group.

This should be comfortably larger than your MID_MAX_PEERS_PER_GROUP
limit in mid_roster.c.

If your limit is very large, increase this value, for example:

    make CFLAGS+="-DLIMIT_MAX_ATTEMPT=100000"
*/
#ifndef LIMIT_MAX_ATTEMPT
#define LIMIT_MAX_ATTEMPT 20000
#endif

#ifndef LIMIT_CHUNK
#define LIMIT_CHUNK 1000
#endif

#define LIMIT_GROUPS 3


/*
Keep inserting peers until the roster count stops growing.

Returns true if a cap was detected.
Stores the capped peer count in *out_count.
*/
static bool group_hits_cap(MidState *s,
                           Tox *tox,
                           uint32_t group_number,
                           const uint8_t *chat_id,
                           size_t *out_count)
{
    size_t prev = 0;
    uint32_t peer_id = 0;
    bool capped = false;

    while (peer_id < LIMIT_MAX_ATTEMPT) {
        uint32_t end = peer_id + LIMIT_CHUNK;

        if (end > LIMIT_MAX_ATTEMPT) {
            end = LIMIT_MAX_ATTEMPT;
        }

        for (; peer_id < end; peer_id++) {
            mid_on_group_peer_join(s, tox, group_number, peer_id);
        }

        size_t cur = mid_peer_count(s, chat_id);

        /*
        If a full chunk of new peer joins did not increase the count,
        the peer limit has been reached.
        */
        if (cur == prev) {
            capped = true;
            break;
        }

        prev = cur;
    }

    /*
    If the limit was reached exactly at LIMIT_MAX_ATTEMPT, the loop above
    may not have observed a stable chunk yet. Try one more chunk.
    */
    if (!capped) {
        size_t before = mid_peer_count(s, chat_id);

        for (uint32_t i = 0; i < LIMIT_CHUNK; i++) {
            mid_on_group_peer_join(s, tox, group_number, peer_id + i);
        }

        size_t after = mid_peer_count(s, chat_id);

        if (after == before) {
            capped = true;
        }

        prev = after;
    }

    if (out_count != NULL) {
        *out_count = prev;
    }

    return capped;
}

static bool test_peer_limit_is_enforced(void)
{
    /*
    Churn mode gives every peer_id a distinct identity key.
    Without this, all peers would have the same key and the roster
    would not grow.
    */
    mock_set_churn(1);

    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();

    T_ASSERT_PTR_NOT_NULL(s, "mid_new");
    T_ASSERT_PTR_NOT_NULL(tox, "create_dummy_tox");

    uint32_t group_numbers[LIMIT_GROUPS] = {
        9101,
        9102,
        9103
    };

/* chat_ids must match what tox_group_get_chat_id() produces for these group_numbers:
     *   byte[0] = group_number & 0xFF
     *   byte[1] = (group_number >> 8) & 0xFF
     */
    uint8_t chat_ids[LIMIT_GROUPS][TOX_GROUP_CHAT_ID_SIZE];
    for (size_t i = 0; i < LIMIT_GROUPS; i++) {
        memset(chat_ids[i], 0, TOX_GROUP_CHAT_ID_SIZE);
        chat_ids[i][0] = (uint8_t)(group_numbers[i] & 0xFF);
        chat_ids[i][1] = (uint8_t)((group_numbers[i] >> 8) & 0xFF);
        chat_ids[i][2] = (uint8_t)((group_numbers[i] >> 16) & 0xFF);
        chat_ids[i][3] = (uint8_t)((group_numbers[i] >> 24) & 0xFF);
    }

    /*
    Create groups.
    */
    for (size_t i = 0; i < LIMIT_GROUPS; i++) {
        mid_on_group_self_join(s, tox, group_numbers[i],
                               (const uint8_t *)"limit", 5);
    }

    T_ASSERT_INT_EQ(mid_group_count(s), LIMIT_GROUPS,
                    "all test groups must be created");

    /*
    Try to store far too many peers in each group.
    */
    for (size_t i = 0; i < LIMIT_GROUPS; i++) {
        size_t stored = 0;

        bool capped = group_hits_cap(s, tox, group_numbers[i], chat_ids[i], &stored);

        T_ASSERT_TRUE(capped,
                      "peer limit must be enforced");

        T_ASSERT_TRUE(stored > 0,
                      "some peers must be stored before the limit is reached");

        /*
        Attempt even more peers after the cap was detected.
        These must be rejected without crashing and without growing the roster.
        */
        uint32_t extra_base = LIMIT_MAX_ATTEMPT + LIMIT_CHUNK + 5000;

        for (uint32_t p = 0; p < 256; p++) {
            mid_on_group_peer_join(s, tox, group_numbers[i], extra_base + p);
        }

        T_ASSERT_INT_EQ(mid_peer_count(s, chat_ids[i]),
                        stored,
                        "peer count must remain capped");

        T_ASSERT_INT_EQ(mid_peer_list_count(s, chat_ids[i]),
                        stored,
                        "peer list count must remain capped");
    }

    mid_free(s);

    mock_set_churn(0);

    return true;
}

int main(void)
{
    TEST_SUITE("midroster peer limit tests");

    RUN_TEST(test_peer_limit_is_enforced);

    SUITE_END();

    return test_summary("midroster_peer_limit");
}

#include "test_framework.h"
#include "../../toxcore/tox.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "../mid_roster.h"

extern Tox *create_dummy_tox(void);

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

static const uint8_t dummy_chat_id_1[TOX_GROUP_CHAT_ID_SIZE] = {1};
static const uint8_t dummy_chat_id_99[TOX_GROUP_CHAT_ID_SIZE] = {99};

bool test_new_and_free(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    T_ASSERT_PTR_NOT_NULL(s, "mid_new failed");
    mid_free(s);
    mid_free(NULL); // Must not crash
    return true;
}

bool test_null_inputs(void) {
    T_ASSERT_INT_EQ(mid_group_count(NULL), 0, "group_count on NULL");
    T_ASSERT_INT_EQ(mid_peer_count(NULL, dummy_chat_id_1), 0, "peer_count on NULL");
    T_ASSERT_INT_EQ(mid_signed_count(NULL, dummy_chat_id_1), 0, "signed_count on NULL");
    T_ASSERT_INT_EQ(mid_online_count(NULL, dummy_chat_id_1), 0, "online_count on NULL");
    T_ASSERT_INT_EQ(mid_find_peer(NULL, dummy_chat_id_1, NULL), -1, "find_peer on NULL");
    T_ASSERT_FALSE(mid_delete_peer_by_identity(NULL, dummy_chat_id_1, NULL), "delete_peer on NULL");
    T_ASSERT_FALSE(mid_peer_is_signed_left(NULL, dummy_chat_id_1, NULL), "is_signed_left on NULL");
    T_ASSERT_FALSE(mid_announce_leave(NULL, NULL, 1), "announce_leave on NULL");

    MidPeerInfo info;
    T_ASSERT_FALSE(mid_peer_list_get(NULL, dummy_chat_id_1, 0, &info), "peer_list_get on NULL state");
    
    mid_iterate(NULL, NULL); // Must not crash
    mid_on_group_self_join(NULL, NULL, 1, NULL, 0);
    mid_on_group_delete(NULL, NULL, 1);
    return true;
}

bool test_extreme_values(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    
    mid_on_group_self_join(s, tox, UINT32_MAX, (uint8_t*)"test", 4);
    T_ASSERT_INT_EQ(mid_group_count(s), 1, "group added");
    mid_on_group_delete(s, tox, UINT32_MAX);
    T_ASSERT_INT_EQ(mid_group_count(s), 0, "group deleted");

    mid_on_group_self_join(s, tox, 1, (uint8_t*)"test", 4);
    mid_on_group_peer_join(s, tox, 1, UINT32_MAX);
    mid_on_group_peer_name(s, tox, 1, UINT32_MAX);
    mid_on_group_peer_exit(s, tox, 1, TOX_GROUP_EXIT_TYPE_QUIT);
    mid_iterate(s, tox);
    
    mid_free(s);
    return true;
}

bool test_empty_nickname(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    mid_on_group_self_join(s, tox, 1, NULL, 0);
    mid_on_group_self_join(s, tox, 2, (uint8_t*)"", 0);
    
    T_ASSERT_INT_EQ(mid_group_count(s), 2, "groups created with empty nicks");
    mid_free(s);
    return true;
}

static int cb_called = 0;
static void test_peer_list_changed_cb(const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], void *user_data) {
    (void)chat_id;
    (void)user_data;
    cb_called++;
}

bool test_callback_and_print(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    
    cb_called = 0;
    mid_set_peer_list_changed_cb(s, test_peer_list_changed_cb, NULL);
    mid_on_group_self_join(s, tox, 1, (uint8_t*)"test", 4);
    
    T_ASSERT_INT_GT(cb_called, 0, "callback should be triggered on self join");
    
    mid_print_peer_table(s, dummy_chat_id_1, "test title");
    mid_print_peer_table(s, dummy_chat_id_99, "missing group");
    
    mid_free(s);
    return true;
}

int main(void) {
    TEST_SUITE("midroster boundary and invariant tests");

    RUN_TEST(test_new_and_free);
    RUN_TEST(test_null_inputs);
    RUN_TEST(test_extreme_values);
    RUN_TEST(test_empty_nickname);
    RUN_TEST(test_callback_and_print);

    SUITE_END();
    return test_summary("midroster_boundaries");
}

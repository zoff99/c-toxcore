/* Tests that we can send messages to friends.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct State {
    uint32_t index;
    uint64_t clock;
    bool peer_joined;
    bool message_sent;
    bool message_received;
} State;

#include "run_auto_test.h"

static void group_invite_handler(Tox *tox, uint32_t friend_number, const uint8_t *invite_data, size_t length,
                                 const uint8_t *group_name, size_t group_name_length, void *user_data)
{
    ck_abort_msg("we should not get invited");
}

static const char *tox_str_group_join_fail(TOX_GROUP_JOIN_FAIL v)
{
    switch (v) {
        case TOX_GROUP_JOIN_FAIL_NAME_TAKEN:
            return "NAME_TAKEN";

        case TOX_GROUP_JOIN_FAIL_PEER_LIMIT:
            return "PEER_LIMIT";

        case TOX_GROUP_JOIN_FAIL_INVALID_PASSWORD:
            return "INVALID_PASSWORD";

        case TOX_GROUP_JOIN_FAIL_UNKNOWN:
            return "UNKNOWN";
    }

    return "<invalid>";
}

static void group_join_fail_handler(Tox *tox, uint32_t groupnumber, TOX_GROUP_JOIN_FAIL fail_type, void *user_data)
{
    printf("join failed: %s\n", tox_str_group_join_fail(fail_type));
}

static State *global_state_tox0;
static State *global_state_tox1;

static void group_peer_join_handler(Tox *tox, uint32_t groupnumber, uint32_t peer_id, void *user_data)
{
    printf("peer %u joined, sending message\n", peer_id);
    global_state_tox1->peer_joined = true;
}

static void group_message_handler(Tox *tox, uint32_t groupnumber, uint32_t peer_id, TOX_MESSAGE_TYPE type,
                                  const uint8_t *message, size_t length, void *user_data)
{
    printf("peer %u sent message: %s\n", peer_id, (const char *)message);
    ck_assert(memcmp(message, "hello", 6) == 0);
    global_state_tox0->message_received = true;
}

static void group_message_test(Tox **toxes, State *state)
{
    tox_self_set_name(toxes[0], (const uint8_t *)"a", 1, nullptr);
    tox_self_set_name(toxes[1], (const uint8_t *)"b", 1, nullptr);

    tox_callback_group_invite(toxes[1], group_invite_handler);
    tox_callback_group_join_fail(toxes[1], group_join_fail_handler);
    tox_callback_group_peer_join(toxes[1], group_peer_join_handler);
    tox_callback_group_message(toxes[0], group_message_handler);

    global_state_tox0 = &state[0];
    global_state_tox1 = &state[1];

    // tox0 makes new group.
    Group_Chat_Self_Peer_Info self_info0;
    self_info0.nick = "tox0";
    self_info0.nick_length = 4;
    self_info0.user_status = TOX_USER_STATUS_NONE;
    TOX_ERR_GROUP_NEW err_new;
    uint32_t group_number =
        tox_group_new(
            toxes[0], TOX_GROUP_PRIVACY_STATE_PUBLIC,
            (const uint8_t *)"my cool group", strlen("my cool group"), &self_info0, &err_new);
    ck_assert(err_new == TOX_ERR_GROUP_NEW_OK);

    // get the chat id of the new group.
    TOX_ERR_GROUP_STATE_QUERIES err_id;
    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    tox_group_get_chat_id(toxes[0], group_number, chat_id, &err_id);
    ck_assert(err_id == TOX_ERR_GROUP_STATE_QUERIES_OK);

    // tox1 joins it.
    Group_Chat_Self_Peer_Info self_info1;
    self_info1.nick = "tox1";
    self_info1.nick_length = 4;
    self_info1.user_status = TOX_USER_STATUS_NONE;
    TOX_ERR_GROUP_JOIN err_join;
    tox_group_join(toxes[1], chat_id, nullptr, 0, &self_info1, &err_join);
    ck_assert(err_join == TOX_ERR_GROUP_JOIN_OK);

    while (!state[0].message_received) {
        tox_iterate(toxes[0], &state[0]);
        tox_iterate(toxes[1], &state[1]);

        if (state[1].peer_joined && !state[1].message_sent) {
            TOX_ERR_GROUP_SEND_MESSAGE err_send;
            tox_group_send_message(toxes[1], group_number, TOX_MESSAGE_TYPE_NORMAL, (const uint8_t *)"hello", 6, &err_send);
            ck_assert(err_send == TOX_ERR_GROUP_SEND_MESSAGE_OK);
            state[1].message_sent = true;
        }

        c_sleep(ITERATION_INTERVAL);
    }
}

int main(void)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    run_auto_test(2, group_message_test, false);
    return 0;
}

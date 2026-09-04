#include "test_framework.h"
#include "../../toxcore/tox.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "../mid_roster.h"

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

extern Tox *create_dummy_tox(void);
extern void mock_set_churn(int on);

#define MID_MSG_ROSTER_REQUEST 2

static _Atomic bool running = true;
static MidState *global_s;
static Tox *global_tox;

/* ── static chat_ids used across threads (for query functions) ── */
static const uint8_t chat_id_1[TOX_GROUP_CHAT_ID_SIZE] = {1};
static const uint8_t chat_id_42[TOX_GROUP_CHAT_ID_SIZE] = {42};
static const uint8_t chat_id_9999[TOX_GROUP_CHAT_ID_SIZE] = {0x99, 0x99};

/* ── group numbers used for the new group_number API ──────────── */
#define GROUP_1    1
#define GROUP_42   42
#define GROUP_9999 0x9999

/* ── helpers ──────────────────────────────────────────────────── */
static void key_from_peer(uint32_t id, uint8_t *out) {
    memset(out, 0, 32);
    out[0] = 0x10;
    out[1] = (uint8_t)(id & 0xFF);
    out[2] = (uint8_t)((id >> 8) & 0xFF);
    out[3] = (uint8_t)((id >> 16) & 0xFF);
}

/* ════════════════════════════════════════════════════════════════
 * TEST 2 — full API churn
 * Each peer_id now has a DISTINCT identity key (mock churn mode),
 * so joins grow the roster, deletes/kicks shrink it, and array
 * shifts race against readers.
 * ════════════════════════════════════════════════════════════════ */

/* mid_iterate */
void *thread_iterate(void *arg) {
    (void)arg;
    while (running) {
        mid_iterate(global_s, global_tox);
    }
    return NULL;
}

/* mid_on_group_self_join + mid_on_group_delete on private groups */
void *thread_group_lifecycle(void *arg) {
    (void)arg;
    uint32_t group = 100;
    while (running) {
        mid_on_group_self_join(global_s, global_tox, group, (uint8_t*)"stress", 6);
        mid_on_group_delete(global_s, global_tox, group);
        group++;
        if (group > 200) group = 100;
    }
    return NULL;
}

/* mid_on_group_peer_join + mid_on_group_peer_name — grows roster */
void *thread_peer_join(void *arg) {
    (void)arg;
    uint32_t id = 0;
    while (running) {
        uint32_t peer_id = 1000 + (id % 64);
        mid_on_group_peer_join(global_s, global_tox, GROUP_1, peer_id);
        mid_on_group_peer_name(global_s, global_tox, GROUP_1, peer_id);
        id++;
    }
    return NULL;
}

/* mid_delete_peer_by_identity — shrinks roster (array shift) */
void *thread_delete(void *arg) {
    (void)arg;
    uint32_t id = 0;
    uint8_t key[32];
    while (running) {
        key_from_peer(1000 + (id % 64), key);
        mid_delete_peer_by_identity(global_s, chat_id_1, key);
        id++;
    }
    return NULL;
}

/* mid_on_group_moderation (KICK) — also shrinks roster */
void *thread_moderation(void *arg) {
    (void)arg;
    uint32_t id = 0;
    while (running) {
        uint32_t target = 1000 + (id % 64);
        mid_on_group_moderation(global_s, global_tox, GROUP_1, 0, target,
                                TOX_GROUP_MOD_EVENT_KICK);
        id++;
    }
    return NULL;
}

/* mid_on_group_peer_exit — triggers mid_sync_online_state_group */
void *thread_peer_exit(void *arg) {
    (void)arg;
    while (running) {
        mid_on_group_peer_exit(global_s, global_tox, GROUP_1, TOX_GROUP_EXIT_TYPE_QUIT);
    }
    return NULL;
}

/* mid_on_group_custom_packet */
/* mid_on_group_custom_packet */
void *thread_custom_packet(void *arg) {
    (void)arg;
    uint8_t roster_req[5] = {
        MID_MAGIC_0,
        MID_MAGIC_1,
        MID_MAGIC_2,
        MID_PROTOCOL_VERSION,
        MID_MSG_ROSTER_REQUEST
    };
    uint8_t garbage[64];
    memset(garbage, 0xFF, sizeof(garbage));
    while (running) {
        mid_on_group_custom_packet(global_s, global_tox, GROUP_1, 1, roster_req, sizeof(roster_req));
        mid_on_group_custom_packet(global_s, global_tox, GROUP_1, 1, garbage, sizeof(garbage));
        mid_on_group_custom_packet(global_s, global_tox, GROUP_1, 1, NULL, 0);
    }
    return NULL;
}

/* query functions */
void *thread_queries(void *arg) {
    (void)arg;
    uint32_t id = 0;
    uint8_t key[32];
    while (running) {
        key_from_peer(1000 + (id % 64), key);
        mid_group_count(global_s);
        mid_peer_count(global_s, chat_id_1);
        mid_signed_count(global_s, chat_id_1);
        mid_online_count(global_s, chat_id_1);
        mid_find_peer(global_s, chat_id_1, key);
        mid_peer_is_signed_left(global_s, chat_id_1, key);
        mid_peer_count(global_s, chat_id_9999);
        id++;
    }
    return NULL;
}

/* mid_peer_list_count + mid_peer_list_get */
void *thread_peer_list(void *arg) {
    (void)arg;
    while (running) {
        size_t count = mid_peer_list_count(global_s, chat_id_1);
        for (size_t i = 0; i < count; i++) {
            MidPeerInfo info;
            mid_peer_list_get(global_s, chat_id_1, i, &info);
        }
        MidPeerInfo info;
        mid_peer_list_get(global_s, chat_id_1, 99999, &info);
        mid_peer_list_get(global_s, chat_id_1, 0, NULL);
    }
    return NULL;
}

/* mid_announce_leave */
void *thread_announce_leave(void *arg) {
    (void)arg;
    while (running) {
        mid_announce_leave(global_s, global_tox, GROUP_1);
        mid_announce_leave(global_s, global_tox, GROUP_9999);
    }
    return NULL;
}

/* mid_set_peer_list_changed_cb + mid_print_peer_table */
static void noop_cb(const uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE], void *user_data) {
    (void)chat_id;
    (void)user_data;
}

void *thread_callback_print(void *arg) {
    (void)arg;
    int toggle = 0;
    while (running) {
        mid_set_peer_list_changed_cb(global_s, (toggle & 1) ? noop_cb : NULL, NULL);
        mid_print_peer_table(global_s, chat_id_1, "stress");
        toggle++;
    }
    return NULL;
}

/* mid_new + mid_free on independent instances */
void *thread_independent_lifecycle(void *arg) {
    (void)arg;
    while (running) {
        MidState *local_s = mid_new(NULL, NULL, 0);
        if (local_s) {
            mid_on_group_self_join(local_s, global_tox, GROUP_42, (uint8_t*)"local", 5);
            mid_iterate(local_s, global_tox);
            mid_peer_count(local_s, chat_id_42);
            mid_on_group_delete(local_s, global_tox, GROUP_42);
            mid_free(local_s);
        }
    }
    return NULL;
}

bool test_all_api_concurrent(void) {
    mock_set_churn(1);   /* distinct identity per peer_id */

    global_s = mid_new(NULL, NULL, 0);
    global_tox = create_dummy_tox();

    mid_on_group_self_join(global_s, global_tox, GROUP_1, (uint8_t*)"test", 4);

    #define NUM_THREADS 11
    pthread_t threads[NUM_THREADS];

    pthread_create(&threads[0],  NULL, thread_iterate,              NULL);
    pthread_create(&threads[1],  NULL, thread_group_lifecycle,      NULL);
    pthread_create(&threads[2],  NULL, thread_peer_join,            NULL);
    pthread_create(&threads[3],  NULL, thread_delete,               NULL);
    pthread_create(&threads[4],  NULL, thread_moderation,           NULL);
    pthread_create(&threads[5],  NULL, thread_peer_exit,            NULL);
    pthread_create(&threads[6],  NULL, thread_custom_packet,        NULL);
    pthread_create(&threads[7],  NULL, thread_queries,              NULL);
    pthread_create(&threads[8],  NULL, thread_peer_list,            NULL);
    pthread_create(&threads[9],  NULL, thread_announce_leave,       NULL);
    pthread_create(&threads[10], NULL, thread_independent_lifecycle, NULL);

    sleep(3);

    running = false;

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    mid_free(global_s);
    mock_set_churn(0);
    return true;
}

int main(void) {
    TEST_SUITE("midroster threading / TSan tests (full API coverage)");

    RUN_TEST(test_all_api_concurrent);

    SUITE_END();
    return test_summary("midroster_threading");
}

#include "test_framework.h"
#include "../../toxcore/tox.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>
#include <stdarg.h>
#include "../mid_roster.h"

#ifndef TOX_GROUP_CHAT_ID_SIZE
#define TOX_GROUP_CHAT_ID_SIZE 32
#endif

extern Tox *create_dummy_tox(void);
extern void mock_set_churn(int on);
extern void mock_set_encrypt_delay_ms(uint32_t ms);

#define MID_MSG_ROSTER_REQUEST 2

/* Simulated scrypt key-derivation cost inside tox_pass_encrypt */
#define SIMULATED_ENC_MS 80

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

/* 
 * Bypass stdout/stderr redirection to force print timing on PASS.
 * Opens /dev/tty directly, bypassing the test framework's pipe/dup2 capture.
 */
static void force_print(const char *fmt, ...) {
    FILE *f = fopen("/dev/tty", "w");
    if (!f) f = stderr; /* fallback if no tty available */
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    if (f != stderr) fclose(f);
    else fflush(f);
}

/* ════════════════════════════════════════════════════════════════
 * TEST 2 — full API churn
 * ════════════════════════════════════════════════════════════════ */

void *thread_iterate(void *arg) {
    (void)arg;
    while (running) {
        mid_iterate(global_s, global_tox);
    }
    return NULL;
}

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

void *thread_peer_exit(void *arg) {
    (void)arg;
    while (running) {
        mid_on_group_peer_exit(global_s, global_tox, GROUP_1, TOX_GROUP_EXIT_TYPE_QUIT);
    }
    return NULL;
}

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

void *thread_announce_leave(void *arg) {
    (void)arg;
    while (running) {
        mid_announce_leave(global_s, global_tox, GROUP_1);
        mid_announce_leave(global_s, global_tox, GROUP_9999);
    }
    return NULL;
}

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
    mock_set_churn(1);
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

/* ════════════════════════════════════════════════════════════════
 * TEST 3 — mid_save heavy concurrency & timing with SLOW encryption
 * ════════════════════════════════════════════════════════════════ */

#define PERF_THREADS          30
#define PERF_CALLS_PER_THREAD 100
#define PERF_TOTAL_OPS        (PERF_THREADS * PERF_CALLS_PER_THREAD)

static _Atomic uint64_t save_min_ns   = UINT64_MAX;
static _Atomic uint64_t save_max_ns   = 0;
static _Atomic uint64_t save_total_ns = 0;
static _Atomic int      save_ok_count = 0;
static _Atomic int      save_fail_count = 0;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void atomic_update_min(_Atomic uint64_t *target, uint64_t val) {
    uint64_t old = atomic_load(target);
    while (val < old && !atomic_compare_exchange_weak(target, &old, val)) {}
}

static void atomic_update_max(_Atomic uint64_t *target, uint64_t val) {
    uint64_t old = atomic_load(target);
    while (val > old && !atomic_compare_exchange_weak(target, &old, val)) {}
}

void *thread_save_perf(void *arg) {
    (void)arg;
    const uint8_t pass[] = "perf_test_pass";
    for (int i = 0; i < PERF_CALLS_PER_THREAD; i++) {
        uint64_t start = get_time_ns();
        bool ok = mid_save(global_s, pass, sizeof(pass) - 1);
        uint64_t elapsed = get_time_ns() - start;

        if (ok) {
            atomic_update_min(&save_min_ns, elapsed);
            atomic_update_max(&save_max_ns, elapsed);
            atomic_fetch_add(&save_total_ns, elapsed);
            atomic_fetch_add(&save_ok_count, 1);
        } else {
            atomic_fetch_add(&save_fail_count, 1);
        }
    }
    return NULL;
}

bool test_save_concurrency_and_timing(void) {
    mock_set_churn(1);
    mock_set_encrypt_delay_ms(SIMULATED_ENC_MS);   /* simulate slow scrypt */

    char save_path[256];
    snprintf(save_path, sizeof(save_path), "/tmp/mid_perf_test_%d.dat", getpid());

    const uint8_t pass[] = "perf_test_pass";

    global_s = mid_new(save_path, pass, sizeof(pass) - 1);
    T_ASSERT_PTR_NOT_NULL(global_s, "mid_new with save_path");

    global_tox = create_dummy_tox();

    mid_on_group_self_join(global_s, global_tox, GROUP_1, (uint8_t*)"perf_user", 9);
    for (int i = 0; i < 100; i++) {
        mid_on_group_peer_join(global_s, global_tox, GROUP_1, 5000 + i);
        mid_on_group_peer_name(global_s, global_tox, GROUP_1, 5000 + i);
    }

    atomic_store(&save_min_ns, UINT64_MAX);
    atomic_store(&save_max_ns, 0);
    atomic_store(&save_total_ns, 0);
    atomic_store(&save_ok_count, 0);
    atomic_store(&save_fail_count, 0);

    /* Using force_print to bypass framework stdout capture on PASS */
    force_print("      Pounding mid_save: %d threads x %d calls = %d total ops...\n",
           PERF_THREADS, PERF_CALLS_PER_THREAD, PERF_TOTAL_OPS);
    force_print("      Simulated encryption delay: %d ms per call\n", SIMULATED_ENC_MS);

    uint64_t wall_start = get_time_ns();

    pthread_t threads[PERF_THREADS];
    for (int i = 0; i < PERF_THREADS; i++) {
        pthread_create(&threads[i], NULL, thread_save_perf, NULL);
    }
    for (int i = 0; i < PERF_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t wall_ms = (get_time_ns() - wall_start) / 1000000ULL;

    int ok_cnt   = atomic_load(&save_ok_count);
    int fail_cnt = atomic_load(&save_fail_count);

    uint64_t serial_wall_ms   = (uint64_t)PERF_TOTAL_OPS * SIMULATED_ENC_MS;
    uint64_t parallel_wall_ms = (uint64_t)PERF_CALLS_PER_THREAD * SIMULATED_ENC_MS;

    force_print("\n      --- mid_save Concurrency Results ---\n");
    force_print("      Successful: %d / %d\n", ok_cnt, PERF_TOTAL_OPS);
    force_print("      Failed:     %d\n", fail_cnt);

    if (ok_cnt > 0) {
        double avg_ms = (double)atomic_load(&save_total_ns) / ok_cnt / 1e6;
        double min_ms = (double)atomic_load(&save_min_ns) / 1e6;
        double max_ms = (double)atomic_load(&save_max_ns) / 1e6;
        force_print("      Time per save (ms): min=%.3f  max=%.3f  avg=%.3f\n",
               min_ms, max_ms, avg_ms);
    }

    force_print("      Wall time: %llu ms\n", (unsigned long long)wall_ms);
    force_print("        (fully serialized would be ~%llu ms)\n",
           (unsigned long long)serial_wall_ms);
    force_print("        (perfect parallel ideal  ~%llu ms)\n",
           (unsigned long long)parallel_wall_ms);

    /* ── Assertions ── */
    T_ASSERT_INT_EQ(fail_cnt, 0, "All mid_save calls succeeded");

    T_ASSERT_TRUE(wall_ms < serial_wall_ms / 4,
                  "Encryption ran concurrently (lock not held during encrypt)");

    if (ok_cnt > 0) {
        double avg_ms = (double)atomic_load(&save_total_ns) / ok_cnt / 1e6;
        T_ASSERT_TRUE(avg_ms < (double)SIMULATED_ENC_MS * 3.0,
                      "Avg save time sane (no heavy lock contention)");
    }

    /* ── Verify file is not corrupt: exists and has reasonable size ── */
    FILE *f = fopen(save_path, "rb");
    T_ASSERT_PTR_NOT_NULL(f, "save file exists after pounding");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fclose(f);
        force_print("      Final file size: %ld bytes\n", fsize);
        T_ASSERT_INT_GT(fsize, 100, "save file not truncated/corrupt (>100 bytes)");
    }

    mid_free(global_s);
    unlink(save_path);
    mock_set_encrypt_delay_ms(0);
    mock_set_churn(0);
    return true;
}

/* ════════════════════════════════════════════════════════════════ */

int main(void) {
    TEST_SUITE("midroster threading / TSan tests (full API coverage)");
    RUN_TEST(test_all_api_concurrent);
    RUN_TEST(test_save_concurrency_and_timing);
    SUITE_END();
    return test_summary("midroster_threading");
}

/*
 * static_test_onion_recovery.c
 *
 * Unit tests for the onion friend startup/recovery helpers:
 *   - immediate DHTPK send + short retry schedule after quorum becomes ready
 *   - startup/recovery lookup seeding and window boundaries
 *   - internal friend diagnostics (live/stored node counts, quorum, counters)
 *
 * Uses the #define static trick to access internal functions from the
 * amalgamation. No network I/O is required.
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Expose all static functions/vars from the amalgamation */
#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

static Mono_Time *test_mono_time(void)
{
    Mono_Time *mt = mono_time_new(NULL, NULL);
    if (mt != NULL) {
        mono_time_update(mt);
    }
    return mt;
}

static void init_fake_friend_node(Onion_Node *node, uint64_t now, bool stored)
{
    memset(node, 0, sizeof(*node));
    node->timestamp = now;
    node->last_pinged = now;
    node->added_time = now;
    node->is_stored = stored ? 1 : 0;
}

bool test_dhtpk_initial_retry_schedule(void)
{
    Onion_Friend f;
    memset(&f, 0, sizeof(f));

    T_ASSERT_FALSE(onion_friend_update_dhtpk_ready(&f, false),
                   "quorum not ready should not report a transition");
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), 0,
                    "no successful send yet: interval is immediate");

    T_ASSERT_TRUE(onion_friend_update_dhtpk_ready(&f, true),
                  "quorum becoming ready should be reported");
    T_ASSERT_INT_EQ(f.dht_pk_onion_send_count, 0,
                    "becoming ready resets the successful-send counter");
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), 0,
                    "first send after quorum ready is immediate");

    f.dht_pk_onion_send_count = 1;
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), 1, "first retry after 1 s");
    f.dht_pk_onion_send_count = 2;
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), 2, "second retry after 2 s");
    f.dht_pk_onion_send_count = 3;
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), 5, "third retry after 5 s");
    f.dht_pk_onion_send_count = 4;
    T_ASSERT_INT_EQ(onion_dhtpk_onion_interval(&f), ONION_DHTPK_SEND_INTERVAL,
                    "after the burst the normal interval is restored");

    T_ASSERT_FALSE(onion_friend_update_dhtpk_ready(&f, true),
                   "staying ready is not a new transition");
    T_ASSERT_INT_EQ(f.dht_pk_onion_send_count, 4,
                    "staying ready must not reset the burst counter");

    T_ASSERT_FALSE(onion_friend_update_dhtpk_ready(&f, false),
                   "losing the quorum is not a become-ready transition");
    T_ASSERT_FALSE(f.dht_pk_onion_ready, "losing the quorum clears readiness");
    T_ASSERT_TRUE(onion_friend_update_dhtpk_ready(&f, true),
                  "regaining the quorum starts a new burst");
    T_ASSERT_INT_EQ(f.dht_pk_onion_send_count, 0,
                    "regaining the quorum resets the burst counter");
    return true;
}

bool test_startup_window_boundaries(void)
{
    Onion_Friend f;
    memset(&f, 0, sizeof(f));

    T_ASSERT_FALSE(onion_friend_startup_active(&f, 100),
                   "no startup window configured");

    f.lookup_startup_until = 120;
    T_ASSERT_TRUE(onion_friend_startup_active(&f, 100), "inside window");
    T_ASSERT_TRUE(onion_friend_startup_active(&f, 119), "last second inside window");
    T_ASSERT_FALSE(onion_friend_startup_active(&f, 120), "window end is exclusive");
    T_ASSERT_FALSE(onion_friend_startup_active(&f, 121), "after window");
    return true;
}

bool test_seed_lookup_resets_backoff(void)
{
    Mono_Time *mt = test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mt, "mono_time_new failed");

    Onion_Client oc;
    memset(&oc, 0, sizeof(oc));
    oc.mono_time = mt;

    Onion_Friend f;
    memset(&f, 0, sizeof(f));
    f.is_valid = true;
    f.run_count = 42;
    f.pings = 99;
    f.time_last_pinged = 12345;
    f.last_populated = 12345;
    f.populate_interval = 77;
    f.dht_pk_onion_send_count = 3;
    f.dht_pk_onion_ready = true;

    oc.num_friends = 1;
    oc.friends_list = &f;

    onion_friend_seed_lookup(&oc, 0);

    const uint64_t now = mono_time_get(mt);
    T_ASSERT_TRUE(f.lookup_seed_pending, "seed marks lookup pending");
    T_ASSERT_INT_EQ(f.lookup_startup_until, now + ONION_FRIEND_STARTUP_WINDOW,
                    "seed starts the aggressive startup window");
    T_ASSERT_INT_EQ(f.populate_interval, ONION_FRIEND_STARTUP_POPULATE_INTERVAL,
                    "seed uses the fast startup populate interval");
    T_ASSERT_INT_EQ(f.last_populated, 0, "seed forces an immediate populate");
    T_ASSERT_INT_EQ(f.run_count, 0, "seed resets lookup backoff");
    T_ASSERT_INT_EQ(f.pings, 0, "seed resets ping counter");
    T_ASSERT_INT_EQ(f.time_last_pinged, 0, "seed resets ping spacing");
    T_ASSERT_INT_EQ(f.dht_pk_onion_send_count, 0, "seed resets DHTPK burst");
    T_ASSERT_FALSE(f.dht_pk_onion_ready, "seed re-evaluates DHTPK readiness");

    /* Online friends are not seeded. */
    f.is_online = true;
    f.lookup_seed_pending = false;
    onion_friend_seed_lookup(&oc, 0);
    T_ASSERT_FALSE(f.lookup_seed_pending, "online friend is not seeded");

    /* Invalid friends are not seeded. */
    f.is_online = false;
    f.is_valid = false;
    onion_friend_seed_lookup(&oc, 0);
    T_ASSERT_FALSE(f.lookup_seed_pending, "invalid friend is not seeded");

    /* Out-of-range friend numbers are ignored. */
    onion_friend_seed_lookup(&oc, 1);

    mono_time_free(mt);
    return true;
}

bool test_addfriend_seeds_lookup(void)
{
    Mono_Time *mt = test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mt, "mono_time_new failed");

    Onion_Client oc;
    memset(&oc, 0, sizeof(oc));
    oc.mono_time = mt;
    oc.rng = system_random();

    uint8_t pk[CRYPTO_PUBLIC_KEY_SIZE];
    memset(pk, 0x42, sizeof(pk));

    const int num = onion_addfriend(&oc, pk);
    T_ASSERT_INT_EQ(num, 0, "onion_addfriend returns friend number");
    T_ASSERT_TRUE(oc.friends_list[0].lookup_seed_pending,
                  "fresh friend add seeds an immediate lookup");
    T_ASSERT_TRUE(oc.friends_list[0].lookup_startup_until > mono_time_get(mt),
                  "fresh friend add starts the startup window");

    /* Adding the same friend again does not create a duplicate. */
    T_ASSERT_INT_EQ(onion_addfriend(&oc, pk), 0, "duplicate add returns same friend");
    T_ASSERT_INT_EQ(oc.num_friends, 1, "duplicate add does not grow friend list");

    realloc_onion_friends(&oc, 0);
    mono_time_free(mt);
    return true;
}

bool test_offline_transition_seeds_lookup(void)
{
    Mono_Time *mt = test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mt, "mono_time_new failed");

    Onion_Client oc;
    memset(&oc, 0, sizeof(oc));
    oc.mono_time = mt;

    Onion_Friend f;
    memset(&f, 0, sizeof(f));
    f.is_valid = true;
    f.is_online = true;
    oc.num_friends = 1;
    oc.friends_list = &f;

    T_ASSERT_INT_EQ(onion_set_friend_online(&oc, 0, false), 0, "set offline succeeds");
    T_ASSERT_TRUE(f.lookup_seed_pending, "online -> offline seeds recovery lookup");
    T_ASSERT_INT_EQ(f.dht_pk_onion_send_count, 0, "offline resets DHTPK burst");
    T_ASSERT_FALSE(f.dht_pk_onion_ready, "offline resets DHTPK readiness");

    /* Repeated offline notifications do not keep re-seeding. */
    f.lookup_seed_pending = false;
    f.lookup_startup_until = 0;
    T_ASSERT_INT_EQ(onion_set_friend_online(&oc, 0, false), 0, "offline again succeeds");
    T_ASSERT_FALSE(f.lookup_seed_pending, "offline -> offline does not re-seed");

    mono_time_free(mt);
    return true;
}

bool test_diagnostics_counts_and_quorum(void)
{
    Mono_Time *mt = test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mt, "mono_time_new failed");

    Onion_Client oc;
    memset(&oc, 0, sizeof(oc));
    oc.mono_time = mt;

    Onion_Friend f;
    memset(&f, 0, sizeof(f));
    f.is_valid = true;
    f.know_dht_public_key = true;
    f.populate_interval = 12;
    f.last_populated = 100;
    f.lookup_seed_pending = true;
    f.lookup_startup_until = 200;
    f.last_dht_pk_onion_sent = 101;
    f.last_dht_pk_dht_sent = 102;
    f.dht_pk_onion_send_count = 2;
    f.last_dht_pk_onion_result = 3;
    f.last_dht_pk_dht_result = -1;
    f.last_dht_pk_recv = 103;
    f.run_count = 4;
    f.pings = 5;

    const uint64_t now = mono_time_get(mt);
    init_fake_friend_node(&f.clients_list[0], now, true);
    init_fake_friend_node(&f.clients_list[1], now, false);
    init_fake_friend_node(&f.clients_list[2], now, true);
    /* Timed-out node must not be counted. */
    init_fake_friend_node(&f.clients_list[3], now, true);
    f.clients_list[3].timestamp = 0;

    oc.num_friends = 1;
    oc.friends_list = &f;

    Onion_Friend_Diagnostics d;
    memset(&d, 0, sizeof(d));
    T_ASSERT_TRUE(onion_get_friend_diagnostics(&oc, 0, &d), "diagnostics succeeds");
    T_ASSERT_INT_EQ(d.friend_num, 0, "diagnostics friend number");
    T_ASSERT_TRUE(d.is_valid, "diagnostics valid flag");
    T_ASSERT_FALSE(d.is_online, "diagnostics online flag");
    T_ASSERT_TRUE(d.know_dht_public_key, "diagnostics DHT key flag");
    T_ASSERT_INT_EQ(d.num_nodes, 3, "live node count excludes timed-out node");
    T_ASSERT_INT_EQ(d.num_stored, 2, "stored node count");
    T_ASSERT_TRUE(d.dhtpk_ready, "2 stored of 3 live satisfies quorum");
    T_ASSERT_INT_EQ(d.last_populated, 100, "diagnostics last populate");
    T_ASSERT_INT_EQ(d.populate_interval, 12, "diagnostics populate interval");
    T_ASSERT_TRUE(d.lookup_seed_pending, "diagnostics seed pending");
    T_ASSERT_INT_EQ(d.lookup_startup_until, 200, "diagnostics startup window");
    T_ASSERT_INT_EQ(d.last_dht_pk_onion_sent, 101, "diagnostics onion send time");
    T_ASSERT_INT_EQ(d.last_dht_pk_dht_sent, 102, "diagnostics DHT send time");
    T_ASSERT_INT_EQ(d.dht_pk_onion_send_count, 2, "diagnostics burst counter");
    T_ASSERT_INT_EQ(d.last_dht_pk_onion_result, 3, "diagnostics onion send result");
    T_ASSERT_INT_EQ(d.last_dht_pk_dht_result, -1, "diagnostics DHT send result");
    T_ASSERT_INT_EQ(d.last_dht_pk_recv, 103, "diagnostics DHTPK receive time");
    T_ASSERT_INT_EQ(d.run_count, 4, "diagnostics run count");
    T_ASSERT_INT_EQ(d.pings, 5, "diagnostics ping count");

    T_ASSERT_FALSE(onion_get_friend_diagnostics(&oc, 1, &d), "out-of-range friend fails");
    T_ASSERT_FALSE(onion_get_friend_diagnostics(&oc, 0, NULL), "NULL output fails");

    mono_time_free(mt);
    return true;
}

int main(void)
{
    TEST_SUITE("onion friend startup/recovery unit tests");

    RUN_TEST(test_dhtpk_initial_retry_schedule);
    RUN_TEST(test_startup_window_boundaries);
    RUN_TEST(test_seed_lookup_resets_backoff);
    RUN_TEST(test_addfriend_seeds_lookup);
    RUN_TEST(test_offline_transition_seeds_lookup);
    RUN_TEST(test_diagnostics_counts_and_quorum);

    SUITE_END();
    return test_summary("onion_recovery");
}

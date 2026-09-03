/*
 * static_test_net_crypto_health_adversarial.c
 *
 * Adversarial / security tests for compute_overall_health().
 * Tests: division by zero, integer overflow, signed/unsigned mismatch,
 * time underflow, boundary values, path coverage, crash resistance.
 *
 * Goal: prove the function NEVER crashes, NEVER does OOB read/write,
 * and handles all edge cases correctly.
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

/* ── Helpers ─────────────────────────────────────────────────────── */

static Net_Crypto *create_test_nc(void)
{
    Mono_Time *mono_time = mono_time_new(NULL, NULL);
    if (!mono_time) return NULL;
    
    Net_Crypto *c = (Net_Crypto *)calloc(1, sizeof(Net_Crypto));
    if (!c) { mono_time_free(mono_time); return NULL; }
    
    c->mono_time = mono_time;
    c->rng = system_random();
    c->tcp_c = (TCP_Connections *)calloc(1, sizeof(TCP_Connections));
    if (!c->tcp_c) { mono_time_free(mono_time); free(c); return NULL; }
    
    c->crypto_connections = NULL;
    c->crypto_connections_length = 0;
    c->overall_health = NET_CRYPTO_HEALTH_UNKNOWN;
    c->overall_health_last_update = 0;
    
    return c;
}

static void destroy_test_nc(Net_Crypto *c)
{
    if (!c) return;
    if (c->crypto_connections) free(c->crypto_connections);
    if (c->tcp_c) free(c->tcp_c);
    if (c->mono_time) mono_time_free(c->mono_time);
    free(c);
}

static Crypto_Connection *add_conn_raw(Net_Crypto *c)
{
    uint32_t idx = c->crypto_connections_length;
    c->crypto_connections = (Crypto_Connection *)realloc(
        c->crypto_connections, (idx + 1) * sizeof(Crypto_Connection));
    if (!c->crypto_connections) return NULL;
    
    Crypto_Connection *conn = &c->crypto_connections[idx];
    memset(conn, 0, sizeof(Crypto_Connection));
    conn->status = CRYPTO_CONN_ESTABLISHED;
    c->crypto_connections_length++;
    return conn;
}

/* ═══════════════════════════════════════════════════════════════════
 * DIVISION BY ZERO TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* total_sent=0, total_resent=0 → idle, skipped (no division) */
bool test_div_zero_both_zero(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    /* total_sent=0, total_resent=0 → idle → continue */
    
    compute_overall_health(c);
    /* Should not crash, transport evaluated → GOOD (TCP relay) */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "idle conn: no division, transport only");
    
    destroy_test_nc(c);
    return true;
}

/* total_sent=0, total_resent>0 → NOT idle, but division guarded */
bool test_div_zero_sent_zero_resent_positive(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    /* total_sent=0, total_resent=50 → not idle (resent>0)
     * enters RTT scoring, but division guarded by total_sent >= 8 */
    conn->last_num_packets_resent[0] = 50;
    /* total_sent stays 0 */
    
    compute_overall_health(c);
    /* Must NOT crash from division by zero */
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash on total_sent=0 with resent>0");
    
    destroy_test_nc(c);
    return true;
}

/* total_sent=1 (below HEALTH_MIN_PACKET_SAMPLE=8), total_resent=1 */
bool test_div_below_min_sample(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 1;
    conn->last_num_packets_resent[0] = 1;
    /* total_sent=1 < 8 → division skipped */
    
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash with total_sent < HEALTH_MIN_PACKET_SAMPLE");
    
    destroy_test_nc(c);
    return true;
}

/* total_sent=7 (just below threshold), total_resent=7 (100% resend) */
bool test_div_just_below_threshold(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 7;
    conn->last_num_packets_resent[0] = 7;
    
    compute_overall_health(c);
    /* total_sent=7 < 8 → division skipped, only RTT scored */
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "total_sent=7 must skip division");
    
    destroy_test_nc(c);
    return true;
}

/* total_sent=8 (exactly at threshold), total_resent=8 (100% resend) */
bool test_div_exactly_at_threshold(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 8;
    conn->last_num_packets_resent[0] = 8;
    /* total_sent=8 >= 8 → division happens: 8*100/8 = 100% → BAD */
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD,
                    "100% resend at exactly threshold should be BAD");
    
    destroy_test_nc(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * INTEGER OVERFLOW TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* total_resent near UINT64_MAX/100 → total_resent*100 could overflow */
bool test_overflow_resent_times_100(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    
    /* Spread huge values across slots to make total_resent very large.
     * CONGESTION_LAST_SENT_ARRAY_SIZE = 24 slots.
     * If each slot has UINT64_MAX/24, total ≈ UINT64_MAX.
     * Then total_resent * 100 would overflow uint64_t. */
    long signed int huge_val = (long signed int)(UINT64_MAX / 30);
    for (unsigned j = 0; j < CONGESTION_LAST_SENT_ARRAY_SIZE; j++) {
        conn->last_num_packets_sent[j] = huge_val;
        conn->last_num_packets_resent[j] = huge_val;
    }
    
    /* Must not crash from integer overflow in (total_resent * 100) */
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash on huge packet counts");
    
    destroy_test_nc(c);
    return true;
}

/* Maximum possible values in all slots */
bool test_overflow_max_values_all_slots(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    
    for (unsigned j = 0; j < CONGESTION_LAST_SENT_ARRAY_SIZE; j++) {
        conn->last_num_packets_sent[j] = LONG_MAX;
        conn->last_num_packets_resent[j] = LONG_MAX;
    }
    
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash on LONG_MAX packet counts");
    
    destroy_test_nc(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * SIGNED/UNSIGNED MISMATCH TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Negative packet counts (signed int) added to uint64_t */
bool test_negative_packet_counts(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = -5;
    conn->last_num_packets_resent[0] = -3;
    /* Negative values cast to uint64_t become huge numbers.
     * total_sent and total_resent will be enormous.
     * Must not crash. */
    
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash on negative packet counts");
    
    destroy_test_nc(c);
    return true;
}

/* Mixed negative and positive counts */
bool test_mixed_negative_positive_counts(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_num_packets_sent[1] = -50;
    conn->last_num_packets_resent[0] = 10;
    conn->last_num_packets_resent[1] = -5;
    
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must not crash on mixed negative/positive counts");
    
    destroy_test_nc(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TIME UNDERFLOW / BOUNDARY TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* last_congestion_event in the future (greater than now) */
bool test_congestion_event_in_future(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    uint64_t now = current_time_monotonic(c->mono_time);
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_congestion_event = now + 999999; /* Future event */
    /* (now - future_event) underflows uint64_t → huge number > 5000
     * So congestion penalty should NOT apply */
    
    compute_overall_health(c);
    /* No congestion penalty → GOOD (TCP relay cap) */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "future congestion event should not trigger penalty");
    
    destroy_test_nc(c);
    return true;
}

/* last_congestion_event = 1 (non-zero, extremely old) */
bool test_congestion_event_ancient(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_congestion_event = 1; /* Very old, (now - 1) >> 5000 */
    
    compute_overall_health(c);
    /* Ancient event → no penalty → GOOD (TCP relay) */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "ancient congestion event should not trigger penalty");
    
    destroy_test_nc(c);
    return true;
}

/* rtt_time = 0 (minimum boundary) */
bool test_rtt_zero(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 0;
    conn->last_num_packets_sent[0] = 100;
    
    compute_overall_health(c);
    /* RTT 0 → EXCELLENT, but TCP relay caps at GOOD */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "RTT 0 with TCP relay should be GOOD");
    
    destroy_test_nc(c);
    return true;
}

/* rtt_time = UINT64_MAX (maximum boundary) */
bool test_rtt_max_uint64(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = UINT64_MAX;
    conn->last_num_packets_sent[0] = 100;
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD,
                    "RTT UINT64_MAX should be BAD");
    
    destroy_test_nc(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * PATH COVERAGE TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* All connections idle → transport evaluated, RTT/resend skipped */
bool test_all_connections_idle(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    for (int i = 0; i < 5; i++) {
        Crypto_Connection *conn = add_conn_raw(c);
        conn->rtt_time = 9999; /* Would be BAD if scored */
        /* No packets → idle */
    }
    
    compute_overall_health(c);
    /* All idle: transport evaluated (GOOD), RTT/resend skipped */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "all idle: transport GOOD, RTT skipped");
    
    destroy_test_nc(c);
    return true;
}

/* Mix of idle and active connections */
bool test_mixed_idle_and_active(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    /* Idle connection with terrible RTT */
    Crypto_Connection *idle = add_conn_raw(c);
    idle->rtt_time = 99999;
    /* No packets → idle, RTT skipped */
    
    /* Active connection with good metrics */
    Crypto_Connection *active = add_conn_raw(c);
    active->rtt_time = 100;
    active->last_num_packets_sent[0] = 100;
    active->last_num_packets_resent[0] = 5;
    
    compute_overall_health(c);
    /* Idle conn skipped, active conn scored: GOOD (TCP relay + good RTT) */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "idle conn RTT must not affect score");
    
    destroy_test_nc(c);
    return true;
}

/* Connection array exists but length is 0 */
bool test_zero_length_with_allocated_array(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    c->crypto_connections = (Crypto_Connection *)calloc(1, sizeof(Crypto_Connection));
    c->crypto_connections_length = 0;
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_UNKNOWN,
                    "length 0 should be UNKNOWN");
    
    destroy_test_nc(c);
    return true;
}

/* Single connection, all metrics at exact threshold boundaries */
bool test_exact_threshold_boundaries(void)
{
    Net_Crypto *c;
    
    /* RTT exactly at HEALTH_RTT_EXCELLENT_MS (200) */
    c = create_test_nc();
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = HEALTH_RTT_EXCELLENT_MS; /* 200 */
    conn->last_num_packets_sent[0] = 100;
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "RTT=200 + TCP relay = GOOD");
    destroy_test_nc(c);
    
    /* RTT exactly at HEALTH_RTT_GOOD_MS (500) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = HEALTH_RTT_GOOD_MS; /* 500 */
    conn->last_num_packets_sent[0] = 100;
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "RTT=500 + TCP relay = GOOD");
    destroy_test_nc(c);
    
    /* RTT exactly at HEALTH_RTT_FAIR_MS (1500) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = HEALTH_RTT_FAIR_MS; /* 1500 */
    conn->last_num_packets_sent[0] = 100;
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_FAIR,
                    "RTT=1500 = FAIR");
    destroy_test_nc(c);
    
    /* RTT exactly at HEALTH_RTT_POOR_MS (4000) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = HEALTH_RTT_POOR_MS; /* 4000 */
    conn->last_num_packets_sent[0] = 100;
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR,
                    "RTT=4000 = POOR");
    destroy_test_nc(c);
    
    /* Resend exactly at HEALTH_RESEND_EXCELLENT_PCT (10%) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_num_packets_resent[0] = 10; /* 10% */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "10% resend + TCP relay = GOOD");
    destroy_test_nc(c);
    
    /* Resend exactly at HEALTH_RESEND_GOOD_PCT (25%) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_num_packets_resent[0] = 25; /* 25% */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "25% resend + TCP relay = GOOD");
    destroy_test_nc(c);
    
    /* Resend exactly at HEALTH_RESEND_FAIR_PCT (50%) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_num_packets_resent[0] = 50; /* 50% */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_FAIR,
                    "50% resend = FAIR");
    destroy_test_nc(c);
    
    /* Resend exactly at HEALTH_RESEND_POOR_PCT (75%) */
    c = create_test_nc();
    conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    conn->last_num_packets_resent[0] = 75; /* 75% */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR,
                    "75% resend = POOR");
    destroy_test_nc(c);
    
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * STRESS / CRASH RESISTANCE TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Many connections (stress test the loop) */
bool test_many_connections(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    for (int i = 0; i < 100; i++) {
        Crypto_Connection *conn = add_conn_raw(c);
        conn->rtt_time = (uint64_t)(i * 50);
        conn->last_num_packets_sent[0] = 100;
        conn->last_num_packets_resent[0] = (long signed int)(i % 20);
    }
    
    compute_overall_health(c);
    /* Must not crash with 100 connections */
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must handle 100 connections without crash");
    
    destroy_test_nc(c);
    return true;
}

/* Rapid repeated calls (throttle + recompute stress) */
bool test_rapid_repeated_calls(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 100;
    
    /* Call 1000 times rapidly - throttle should prevent recomputation */
    for (int i = 0; i < 1000; i++) {
        compute_overall_health(c);
    }
    
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "must handle 1000 rapid calls without crash");
    
    destroy_test_nc(c);
    return true;
}

/* Resend > sent (impossible in reality, but must not crash) */
bool test_resent_greater_than_sent(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    conn->last_num_packets_sent[0] = 10;
    conn->last_num_packets_resent[0] = 500; /* 5000% resend */
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD,
                    "resent > sent should be BAD");
    
    destroy_test_nc(c);
    return true;
}

/* All packet slots filled, only one has data */
bool test_sparse_packet_slots(void)
{
    Net_Crypto *c = create_test_nc();
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");
    
    Crypto_Connection *conn = add_conn_raw(c);
    conn->rtt_time = 100;
    /* Only slot 23 (last) has data */
    conn->last_num_packets_sent[CONGESTION_LAST_SENT_ARRAY_SIZE - 1] = 100;
    conn->last_num_packets_resent[CONGESTION_LAST_SENT_ARRAY_SIZE - 1] = 5;
    
    compute_overall_health(c);
    T_ASSERT_TRUE(c->overall_health >= NET_CRYPTO_HEALTH_EXCELLENT,
                  "sparse slots must not crash");
    
    destroy_test_nc(c);
    return true;
}

int main(void)
{
    TEST_SUITE("compute_overall_health() ADVERSARIAL tests");
    
    /* Division by zero */
    RUN_TEST(test_div_zero_both_zero);
    RUN_TEST(test_div_zero_sent_zero_resent_positive);
    RUN_TEST(test_div_below_min_sample);
    RUN_TEST(test_div_just_below_threshold);
    RUN_TEST(test_div_exactly_at_threshold);
    
    /* Integer overflow */
    RUN_TEST(test_overflow_resent_times_100);
    RUN_TEST(test_overflow_max_values_all_slots);
    
    /* Signed/unsigned mismatch */
    RUN_TEST(test_negative_packet_counts);
    RUN_TEST(test_mixed_negative_positive_counts);
    
    /* Time underflow / boundaries */
    RUN_TEST(test_congestion_event_in_future);
    RUN_TEST(test_congestion_event_ancient);
    RUN_TEST(test_rtt_zero);
    RUN_TEST(test_rtt_max_uint64);
    
    /* Path coverage */
    RUN_TEST(test_all_connections_idle);
    RUN_TEST(test_mixed_idle_and_active);
    RUN_TEST(test_zero_length_with_allocated_array);
    RUN_TEST(test_exact_threshold_boundaries);
    
    /* Stress / crash resistance */
    RUN_TEST(test_many_connections);
    RUN_TEST(test_rapid_repeated_calls);
    RUN_TEST(test_resent_greater_than_sent);
    RUN_TEST(test_sparse_packet_slots);
    
    SUITE_END();
    return test_summary("net_crypto_health_adversarial");
}

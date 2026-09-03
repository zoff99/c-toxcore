/*
 * static_test_net_crypto_health.c
 *
 * Unit tests for compute_overall_health() in net_crypto.c
 * Uses the #define static trick to access internal functions.
 *
 * NOTE: crypto_connection_status() requires full networking state that we
 * cannot easily mock. In our test environment, all connections are treated
 * as TCP relay (direct=false), which caps health at GOOD.
 * We test:
 *   - UNKNOWN when no connections
 *   - Throttling behavior
 *   - TCP relay cap at GOOD
 *   - RTT thresholds that override the cap (FAIR, POOR, BAD)
 *   - Resend ratio thresholds that override the cap
 *   - Congestion penalty
 *   - Idle connection skipping
 *   - Insufficient packet sample
 *   - Multiple connections worst-wins
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* Expose all static functions/vars from the amalgamation */
#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

/* Helper: create a minimal Net_Crypto instance for testing */
static Net_Crypto *create_test_net_crypto(void)
{
    Mono_Time *mono_time = mono_time_new(NULL, NULL);
    if (!mono_time) return NULL;
    
    Net_Crypto *c = (Net_Crypto *)calloc(1, sizeof(Net_Crypto));
    if (!c) {
        mono_time_free(mono_time);
        return NULL;
    }
    
    c->mono_time = mono_time;
    c->log = NULL;
    c->rng = system_random();
    
    c->tcp_c = (TCP_Connections *)calloc(1, sizeof(TCP_Connections));
    if (!c->tcp_c) {
        mono_time_free(mono_time);
        free(c);
        return NULL;
    }
    
    c->crypto_connections = NULL;
    c->crypto_connections_length = 0;
    c->overall_health = NET_CRYPTO_HEALTH_UNKNOWN;
    c->overall_health_last_update = 0;
    
    return c;
}

static void destroy_test_net_crypto(Net_Crypto *c)
{
    if (!c) return;
    if (c->crypto_connections) free(c->crypto_connections);
    if (c->tcp_c) free(c->tcp_c);
    if (c->mono_time) mono_time_free(c->mono_time);
    free(c);
}

/* Helper: add an established connection with given parameters */
static Crypto_Connection *add_test_connection(
    Net_Crypto *c,
    uint64_t rtt_ms,
    uint64_t last_congestion_event,
    uint64_t total_sent,
    uint64_t total_resent)
{
    uint32_t new_idx = c->crypto_connections_length;
    c->crypto_connections = (Crypto_Connection *)realloc(
        c->crypto_connections,
        (new_idx + 1) * sizeof(Crypto_Connection));
    
    if (!c->crypto_connections) return NULL;
    
    Crypto_Connection *conn = &c->crypto_connections[new_idx];
    memset(conn, 0, sizeof(Crypto_Connection));
    
    conn->status = CRYPTO_CONN_ESTABLISHED;
    conn->rtt_time = rtt_ms;
    conn->last_congestion_event = last_congestion_event;
    
    if (total_sent > 0) {
        conn->last_num_packets_sent[0] = (long signed int)total_sent;
    }
    if (total_resent > 0) {
        conn->last_num_packets_resent[0] = (long signed int)total_resent;
    }
    
    /* TCP relay only (no direct UDP) - crypto_connection_status needs
     * full networking state we can't mock, so all connections are relay */
    conn->ip_portv4.ip.family.value = 0;
    conn->direct_lastrecv_timev4 = 0;
    
    c->crypto_connections_length++;
    return conn;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS
 * ═══════════════════════════════════════════════════════════════════ */

bool test_health_no_connections(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_UNKNOWN,
                    "no connections should be UNKNOWN");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_non_established_connections(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    c->crypto_connections = (Crypto_Connection *)calloc(1, sizeof(Crypto_Connection));
    c->crypto_connections_length = 1;
    c->crypto_connections[0].status = CRYPTO_CONN_COOKIE_REQUESTING;
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_UNKNOWN,
                    "non-established connections should be UNKNOWN");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_throttling(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* TCP relay + RTT 100ms + 5% resend -> GOOD (TCP relay cap) */
    add_test_connection(c, 100, 0, 100, 5);
    
    compute_overall_health(c);
    Net_Crypto_Health first_result = c->overall_health;
    uint64_t first_update = c->overall_health_last_update;
    
    T_ASSERT_TRUE(first_result != NET_CRYPTO_HEALTH_UNKNOWN,
                  "first call should compute a result");
    
    /* Degrade the connection */
    c->crypto_connections[0].rtt_time = 5000;
    
    /* Immediate second call should be throttled */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health_last_update, first_update,
                    "immediate second call should be throttled");
    T_ASSERT_INT_EQ(c->overall_health, first_result,
                    "throttled call should not recompute");
    
    /* Clear throttle to allow recomputation */
    c->overall_health_last_update = 0;
    
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD,
                    "after throttle expires, should recompute to BAD");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_tcp_relay_caps_at_good(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* Excellent metrics but TCP relay -> capped at GOOD */
    add_test_connection(c, 100, 0, 100, 5);
    compute_overall_health(c);
    
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "TCP relay with excellent metrics should be capped at GOOD");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_rtt_fair(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 501, 0, 100, 5);
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_FAIR, "RTT 501ms should be FAIR");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_rtt_poor(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 1501, 0, 100, 5);
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR, "RTT 1501ms should be POOR");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_rtt_bad(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 4001, 0, 100, 5);
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD, "RTT 4001ms should be BAD");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_resend_ratio_fair(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 100, 0, 100, 26); /* 26% resend */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_FAIR, "26% resend should be FAIR");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_resend_ratio_poor(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 100, 0, 100, 51); /* 51% resend */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR, "51% resend should be POOR");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_resend_ratio_bad(void)
{
    Net_Crypto *c = create_test_net_crypto();
    add_test_connection(c, 100, 0, 100, 76); /* 76% resend */
    compute_overall_health(c);
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD, "76% resend should be BAD");
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_congestion_penalty(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* last_congestion_event must be NON-ZERO (0 means "never happened").
     * Set it to current time so (now - last_congestion_event) < 5000ms. */
    uint64_t now = current_time_monotonic(c->mono_time);
    add_test_connection(c, 100, now, 100, 5);
    compute_overall_health(c);
    
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR,
                    "recent congestion should cap at POOR");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_idle_connection_skipped(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* Idle connection (0 sent, 0 resent) with stale high RTT.
     * Transport is still evaluated (TCP relay -> GOOD).
     * RTT/resend are skipped because connection is idle.
     * Result: GOOD (from TCP relay), NOT UNKNOWN (connection IS established). */
    add_test_connection(c, 5000, 0, 0, 0);
    compute_overall_health(c);
    
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "idle connection: transport evaluated (GOOD), RTT/resend skipped");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_insufficient_packet_sample(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* Only 5 packets sent (< HEALTH_MIN_PACKET_SAMPLE = 8) */
    /* High resend (80%) should be ignored */
    add_test_connection(c, 100, 0, 5, 4);
    compute_overall_health(c);
    
    /* TCP relay caps at GOOD, RTT 100ms is EXCELLENT, resend skipped */
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_GOOD,
                    "insufficient packet sample should skip resend ratio, TCP relay caps at GOOD");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_multiple_connections_worst_wins(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    add_test_connection(c, 100, 0, 100, 5);   /* GOOD (TCP relay cap) */
    add_test_connection(c, 600, 0, 100, 5);   /* FAIR */
    add_test_connection(c, 3000, 0, 100, 5);  /* POOR */
    
    compute_overall_health(c);
    
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_POOR,
                    "multiple connections should use worst score");
    
    destroy_test_net_crypto(c);
    return true;
}

bool test_health_worst_connection_determines_score(void)
{
    Net_Crypto *c = create_test_net_crypto();
    T_ASSERT_PTR_NOT_NULL(c, "create_test_net_crypto failed");
    
    /* One good connection, one bad connection */
    add_test_connection(c, 100, 0, 100, 5);   /* GOOD */
    add_test_connection(c, 5000, 0, 100, 5);  /* BAD */
    
    compute_overall_health(c);
    
    T_ASSERT_INT_EQ(c->overall_health, NET_CRYPTO_HEALTH_BAD,
                    "worst connection should determine overall score");
    
    destroy_test_net_crypto(c);
    return true;
}

int main(void)
{
    TEST_SUITE("compute_overall_health() unit tests");
    
    RUN_TEST(test_health_no_connections);
    RUN_TEST(test_health_non_established_connections);
    RUN_TEST(test_health_throttling);
    RUN_TEST(test_health_tcp_relay_caps_at_good);
    RUN_TEST(test_health_rtt_fair);
    RUN_TEST(test_health_rtt_poor);
    RUN_TEST(test_health_rtt_bad);
    RUN_TEST(test_health_resend_ratio_fair);
    RUN_TEST(test_health_resend_ratio_poor);
    RUN_TEST(test_health_resend_ratio_bad);
    RUN_TEST(test_health_congestion_penalty);
    RUN_TEST(test_health_idle_connection_skipped);
    RUN_TEST(test_health_insufficient_packet_sample);
    RUN_TEST(test_health_multiple_connections_worst_wins);
    RUN_TEST(test_health_worst_connection_determines_score);
    
    SUITE_END();
    return test_summary("net_crypto_health");
}

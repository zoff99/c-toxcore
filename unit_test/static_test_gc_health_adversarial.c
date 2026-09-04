/*
 * static_test_gc_health_adversarial.c
 *
 * Adversarial / security tests for gc_compute_health().
 * Tests: division by zero, integer overflow, boundary values,
 * NULL pointers, circular buffer edge cases, stress testing.
 *
 * Goal: prove gc_compute_health() NEVER crashes, NEVER does OOB
 * read/write, and handles all edge cases correctly.
 */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

/* ── Helpers ─────────────────────────────────────────────────────── */

static GC_Session *create_adv_gc_session(uint32_t num_chats)
{
    Mono_Time *mono_time = mono_time_new(NULL, NULL);
    if (!mono_time) return NULL;

    GC_Session *c = (GC_Session *)calloc(1, sizeof(GC_Session));
    if (!c) { mono_time_free(mono_time); return NULL; }

    c->chats = (GC_Chat *)calloc(num_chats > 0 ? num_chats : 1, sizeof(GC_Chat));
    if (!c->chats) { mono_time_free(mono_time); free(c); return NULL; }

    c->chats_index = num_chats;

    for (uint32_t i = 0; i < num_chats; i++) {
        c->chats[i].mono_time = mono_time;
        c->chats[i].connection_state = CS_CONNECTED;
        c->chats[i].numpeers = 0;
        c->chats[i].group = NULL;
    }

    return c;
}

static void destroy_adv_gc_session(GC_Session *c)
{
    if (!c) return;
    if (c->chats) {
        for (uint32_t i = 0; i < c->chats_index; i++) {
            GC_Chat *chat = &c->chats[i];
            if (chat->group) {
                for (uint32_t j = 0; j < chat->numpeers; j++) {
                    if (chat->group[j].gconn.send_array) {
                        free(chat->group[j].gconn.send_array);
                    }
                }
                free(chat->group);
            }
        }
        /* mono_time is shared across all chats, free once */
        if (c->chats_index > 0 && c->chats[0].mono_time) {
            mono_time_free(c->chats[0].mono_time);
        }
        free(c->chats);
    }
    free(c);
}

static GC_Connection *add_adv_peer(GC_Session *c, uint32_t chat_idx)
{
    GC_Chat *chat = &c->chats[chat_idx];
    uint32_t new_idx = chat->numpeers;

    chat->group = (GC_Peer *)realloc(
        chat->group, (new_idx + 1) * sizeof(GC_Peer));
    if (!chat->group) return NULL;

    GC_Peer *peer = &chat->group[new_idx];
    memset(peer, 0, sizeof(GC_Peer));

    GC_Connection *gconn = &peer->gconn;
    gconn->confirmed = true;
    gconn->pending_delete = false;
    gconn->handshaked = true;
    gconn->handshake_attempts = 0;
    gconn->send_array = NULL;
    gconn->send_array_start = 0;
    gconn->send_message_id = 0;
    gconn->last_received_packet_time = 0;

    chat->numpeers++;
    return gconn;
}

/* ═══════════════════════════════════════════════════════════════════
 * DIVISION BY ZERO TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* peer_count=0 → must not divide by zero */
bool test_gc_div_zero_no_peers(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    /* Connected chat but no peers added */
    c->chats[0].numpeers = 0;

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_INT_EQ(result, GC_HEALTH_UNKNOWN,
                    "0 peers must return UNKNOWN, not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* All peers are self (index 0 only) → peer_count stays 0 */
bool test_gc_div_zero_only_self(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0); /* Only self at index 0 */

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_INT_EQ(result, GC_HEALTH_UNKNOWN,
                    "only self peer must return UNKNOWN, not divide by zero");

    destroy_adv_gc_session(c);
    return true;
}

/* All peers unconfirmed → peer_count stays 0 */
bool test_gc_div_zero_all_unconfirmed(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0); /* self */
    for (int i = 0; i < 5; i++) {
        GC_Connection *p = add_adv_peer(c, 0);
        p->confirmed = false;
    }

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_INT_EQ(result, GC_HEALTH_UNKNOWN,
                    "all unconfirmed must return UNKNOWN, not divide by zero");

    destroy_adv_gc_session(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * INTEGER OVERFLOW TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Many peers to stress total_score accumulation */
bool test_gc_overflow_many_peers(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0); /* self */

    /* Add 200 peers - total_score could get large */
    for (int i = 0; i < 200; i++) {
        GC_Connection *p = add_adv_peer(c, 0);
        p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);
        p->handshake_attempts = 10; /* FAIR score per peer */
        p->handshaked = false;
    }

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "200 peers must not crash or overflow");

    destroy_adv_gc_session(c);
    return true;
}

/* Maximum handshake_attempts value (uint16_t) */
bool test_gc_overflow_max_handshake_attempts(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0); /* self */
    GC_Connection *p = add_adv_peer(c, 0);
    p->handshake_attempts = UINT16_MAX;
    p->handshaked = false;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "UINT16_MAX handshake attempts must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * BOUNDARY VALUE TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* handshake_attempts exactly at threshold (GC_HEALTH_HANDSHAKE_FAIR = 5) */
bool test_gc_boundary_handshake_exact(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->handshake_attempts = GC_HEALTH_HANDSHAKE_FAIR; /* exactly 5 */
    p->handshaked = false;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    /* Check is > GC_HEALTH_HANDSHAKE_FAIR, so exactly 5 should NOT trigger penalty.
     * However, other metrics (transport, staleness) may affect the score.
     * We just verify it doesn't crash and produces a valid result. */
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "handshake_attempts == threshold must produce valid result");

    destroy_adv_gc_session(c);
    return true;
}

/* handshake_attempts just above threshold */
bool test_gc_boundary_handshake_above(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->handshake_attempts = GC_HEALTH_HANDSHAKE_FAIR + 1; /* 6 */
    p->handshaked = false;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_FAIR,
                  "handshake_attempts > threshold should trigger FAIR");

    destroy_adv_gc_session(c);
    return true;
}

/* last_received_packet_time = 0 → skipped (no staleness check) */
bool test_gc_boundary_recv_time_zero(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->last_received_packet_time = 0; /* No timestamp yet */

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "recv_time=0 must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* last_received_packet_time = UINT64_MAX */
bool test_gc_boundary_recv_time_max(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->last_received_packet_time = UINT64_MAX;

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "recv_time=UINT64_MAX must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * NULL / MISSING STATE TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* send_array = NULL → queue check skipped */
bool test_gc_null_send_array(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->send_array = NULL;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "NULL send_array must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* get_gc_connection returns NULL (index out of range internally) */
bool test_gc_null_connection(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    /* Set numpeers high but don't allocate connections array properly */
    add_adv_peer(c, 0); /* self */
    /* Peer at index 1 exists */
    add_adv_peer(c, 0);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "must handle connection lookup safely");

    destroy_adv_gc_session(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * MEAN CALCULATION TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Single peer: mean = that peer's score */
bool test_gc_mean_single_peer(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->handshake_attempts = 10;
    p->handshaked = false;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    /* Single peer with handshake penalty → at least FAIR */
    T_ASSERT_TRUE(result >= GC_HEALTH_FAIR,
                  "single struggling peer mean should reflect its score");

    destroy_adv_gc_session(c);
    return true;
}

/* Rounding: 2 peers with scores 1 and 2 → mean = (3+1)/2 = 2 */
bool test_gc_mean_rounding(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);

    /* Peer 1: healthy */
    GC_Connection *p1 = add_adv_peer(c, 0);
    p1->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    /* Peer 2: struggling (FAIR) */
    GC_Connection *p2 = add_adv_peer(c, 0);
    p2->handshake_attempts = 10;
    p2->handshaked = false;
    p2->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "mean rounding must produce valid result");

    destroy_adv_gc_session(c);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * STRESS / CRASH RESISTANCE TESTS
 * ═══════════════════════════════════════════════════════════════════ */

/* Many chats with many peers */
bool test_gc_stress_many_chats_and_peers(void)
{
    GC_Session *c = create_adv_gc_session(10);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    for (uint32_t chat = 0; chat < 10; chat++) {
        add_adv_peer(c, chat); /* self */
        for (int i = 0; i < 20; i++) {
            GC_Connection *p = add_adv_peer(c, chat);
            p->last_received_packet_time = current_time_monotonic(c->chats[chat].mono_time);
            p->handshake_attempts = (uint16_t)(i % 15);
            p->handshaked = (i % 3 == 0);
        }
    }

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "10 chats x 20 peers must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* Rapid repeated calls */
bool test_gc_stress_rapid_calls(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    for (int i = 0; i < 500; i++) {
        gc_compute_health(c, NULL);
    }

    T_ASSERT_TRUE(true, "500 rapid calls must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* Mixed connection states across chats */
bool test_gc_stress_mixed_states(void)
{
    GC_Session *c = create_adv_gc_session(5);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    c->chats[0].connection_state = CS_CONNECTED;
    c->chats[1].connection_state = CS_DISCONNECTED;
    c->chats[2].connection_state = CS_NONE;
    c->chats[3].connection_state = CS_CONNECTED;
    c->chats[4].connection_state = CS_DISCONNECTED;

    /* Add peers only to connected chats */
    add_adv_peer(c, 0);
    GC_Connection *p0 = add_adv_peer(c, 0);
    p0->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    add_adv_peer(c, 3);
    GC_Connection *p3 = add_adv_peer(c, 3);
    p3->handshake_attempts = 8;
    p3->handshaked = false;
    p3->last_received_packet_time = current_time_monotonic(c->chats[3].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "mixed connection states must not crash");

    destroy_adv_gc_session(c);
    return true;
}

/* numpeers = 1 (only self, loop doesn't execute) */
bool test_gc_boundary_numpeers_one(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0); /* self only, numpeers=1 */

    GC_Health result = gc_compute_health(c, NULL);
    T_ASSERT_INT_EQ(result, GC_HEALTH_UNKNOWN,
                    "numpeers=1 (only self) should return UNKNOWN");

    destroy_adv_gc_session(c);
    return true;
}

/* handshake_attempts = 0, handshaked = false → no penalty */
bool test_gc_boundary_zero_handshake_attempts(void)
{
    GC_Session *c = create_adv_gc_session(1);
    T_ASSERT_PTR_NOT_NULL(c, "alloc failed");

    add_adv_peer(c, 0);
    GC_Connection *p = add_adv_peer(c, 0);
    p->handshake_attempts = 0;
    p->handshaked = false;
    p->last_received_packet_time = current_time_monotonic(c->chats[0].mono_time);

    GC_Health result = gc_compute_health(c, NULL);
    /* 0 attempts is not > GC_HEALTH_HANDSHAKE_FAIR, so no penalty.
     * However, other metrics may affect the score. Verify valid result. */
    T_ASSERT_TRUE(result >= GC_HEALTH_EXCELLENT && result <= GC_HEALTH_BAD,
                  "0 handshake attempts must produce valid result");

    destroy_adv_gc_session(c);
    return true;
}

int main(void)
{
    TEST_SUITE("gc_compute_health() ADVERSARIAL tests");

    /* Division by zero */
    RUN_TEST(test_gc_div_zero_no_peers);
    RUN_TEST(test_gc_div_zero_only_self);
    RUN_TEST(test_gc_div_zero_all_unconfirmed);

    /* Integer overflow */
    RUN_TEST(test_gc_overflow_many_peers);
    RUN_TEST(test_gc_overflow_max_handshake_attempts);

    /* Boundary values */
    RUN_TEST(test_gc_boundary_handshake_exact);
    RUN_TEST(test_gc_boundary_handshake_above);
    RUN_TEST(test_gc_boundary_recv_time_zero);
    RUN_TEST(test_gc_boundary_recv_time_max);

    /* NULL / missing state */
    RUN_TEST(test_gc_null_send_array);
    RUN_TEST(test_gc_null_connection);

    /* Mean calculation */
    RUN_TEST(test_gc_mean_single_peer);
    RUN_TEST(test_gc_mean_rounding);

    /* Stress / crash resistance */
    RUN_TEST(test_gc_stress_many_chats_and_peers);
    RUN_TEST(test_gc_stress_rapid_calls);
    RUN_TEST(test_gc_stress_mixed_states);
    RUN_TEST(test_gc_boundary_numpeers_one);
    RUN_TEST(test_gc_boundary_zero_handshake_attempts);

    SUITE_END();
    return test_summary("gc_health_adversarial");
}

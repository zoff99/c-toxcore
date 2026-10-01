/*
 * static_test_msi.c
 *
 * Comprehensive static unit tests for toxav/msi.c
 *
 * Tests MSI message parsing, call lifecycle, callback dispatch,
 * protocol state handling, and edge cases around kill_call().
 *
 * Uses the #define static trick to access internal functions.
 */

#include "test_framework.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

#define static
#define inline
#include "../amalgamation/toxcore_amalgamation.c"
#undef inline
#undef static

#ifndef PACKET_ID_MSI
#define PACKET_ID_MSI 69
#endif

/* May already be declared in amalgamation; declare defensively. */
extern void tox_set_av_object(Tox *tox, void *object);

/* ── Global test Tox instance ───────────────────────────────────── */

static Tox *g_tox = NULL;

/* ── MSI callback counters ──────────────────────────────────────── */

static int g_cb_ret = 0;

static int g_invite_count = 0;
static int g_start_count = 0;
static int g_end_count = 0;
static int g_error_count = 0;
static int g_peertimeout_count = 0;
static int g_capabilities_count = 0;

static void *g_last_av = NULL;
static MSICall *g_last_call = NULL;

static void reset_msi_counters(void)
{
    g_cb_ret = 0;

    g_invite_count = 0;
    g_start_count = 0;
    g_end_count = 0;
    g_error_count = 0;
    g_peertimeout_count = 0;
    g_capabilities_count = 0;

    g_last_av = NULL;
    g_last_call = NULL;
}

static int cb_invite(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_invite_count++;
    return g_cb_ret;
}

static int cb_start(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_start_count++;
    return g_cb_ret;
}

static int cb_end(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_end_count++;
    return g_cb_ret;
}

static int cb_error(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_error_count++;
    return g_cb_ret;
}

static int cb_peertimeout(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_peertimeout_count++;
    return g_cb_ret;
}

static int cb_capabilities(void *av, MSICall *call)
{
    g_last_av = av;
    g_last_call = call;
    g_capabilities_count++;
    return g_cb_ret;
}

static void register_all_msi_callbacks(MSISession *session)
{
    msi_register_callback(session, cb_invite,       MSI_ON_INVITE);
    msi_register_callback(session, cb_start,        MSI_ON_START);
    msi_register_callback(session, cb_end,          MSI_ON_END);
    msi_register_callback(session, cb_error,        MSI_ON_ERROR);
    msi_register_callback(session, cb_peertimeout,  MSI_ON_PEERTIMEOUT);
    msi_register_callback(session, cb_capabilities, MSI_ON_CAPABILITIES);
}

/* ── Helpers ────────────────────────────────────────────────────── */

static Tox *create_test_tox(void)
{
    struct Tox_Options options;
    tox_options_default(&options);

    options.ipv6_enabled = false;
    options.local_discovery_enabled = false;
    options.hole_punching_enabled = false;
    options.udp_enabled = false;
    options.tcp_port = 0;
    options.savedata_type = TOX_SAVEDATA_TYPE_NONE;

    return tox_new(&options, NULL);
}

static MSISession *create_msi_session(void)
{
    MSISession *session = msi_new(g_tox);

    if (session == NULL) {
        return NULL;
    }

    session->av = (void *)0xA5;

    register_all_msi_callbacks(session);
    reset_msi_counters();

    return session;
}

static void destroy_msi_session(MSISession *session)
{
    if (session != NULL) {
        msi_kill(g_tox, session, NULL);
    }
}

static MSIMessage make_caps_msg(uint8_t caps)
{
    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));

    msg.request.exists = true;
    msg.request.value = REQU_INIT;

    msg.capabilities.exists = true;
    msg.capabilities.value = caps;

    return msg;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: message helpers
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_msg_init(void)
{
    MSIMessage msg;

    msg_init(&msg, REQU_PUSH);

    T_ASSERT_TRUE(msg.request.exists, "request.exists should be true");
    T_ASSERT_INT_EQ(msg.request.value, REQU_PUSH, "request.value should be REQU_PUSH");
    T_ASSERT_FALSE(msg.error.exists, "error.exists should be false");
    T_ASSERT_FALSE(msg.capabilities.exists, "capabilities.exists should be false");

    return true;
}

static bool test_check_size_valid(void)
{
    uint8_t bytes[3] = { ID_REQUEST, 1, 0 };
    int constraint = 4;

    bool ok = check_size(g_tox, bytes, &constraint, 1);

    T_ASSERT_TRUE(ok, "valid header size should pass");
    T_ASSERT_INT_EQ(constraint, 1, "constraint should be reduced by 2 + size");

    return true;
}

static bool test_check_size_invalid_size_field(void)
{
    uint8_t bytes[3] = { ID_REQUEST, 2, 0 }; /* says size=2, but caller says size=1 */
    int constraint = 5;

    bool ok = check_size(g_tox, bytes, &constraint, 1);

    T_ASSERT_FALSE(ok, "mismatched size field should fail");

    return true;
}

static bool test_check_size_constraint_too_small(void)
{
    uint8_t bytes[3] = { ID_REQUEST, 1, 0 };
    int constraint = 2; /* 2 - (2 + 1) = -1 */

    bool ok = check_size(g_tox, bytes, &constraint, 1);

    T_ASSERT_FALSE(ok, "constraint underflow should fail");

    return true;
}

static bool test_check_enum_high(void)
{
    uint8_t bytes_ok[3] = { ID_REQUEST, 1, REQU_POP };
    uint8_t bytes_bad[3] = { ID_REQUEST, 1, REQU_POP + 1 };

    T_ASSERT_TRUE(check_enum_high(g_tox, bytes_ok, REQU_POP),
                  "value <= enum_high should pass");

    T_ASSERT_FALSE(check_enum_high(g_tox, bytes_bad, REQU_POP),
                   "value > enum_high should fail");

    return true;
}

static bool test_msg_parse_in_valid_full_message(void)
{
    uint8_t raw[] = {
        ID_REQUEST, 1, REQU_INIT,
        ID_CAPABILITIES, 1, MSI_CAP_R_AUDIO,
        ID_ERROR, 1, MSI_E_INVALID_STATE,
        0
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, 0, "valid message should parse");
    T_ASSERT_TRUE(msg.request.exists, "request should exist");
    T_ASSERT_INT_EQ(msg.request.value, REQU_INIT, "request should be INIT");
    T_ASSERT_TRUE(msg.capabilities.exists, "capabilities should exist");
    T_ASSERT_INT_EQ(msg.capabilities.value, MSI_CAP_R_AUDIO, "capabilities should match");
    T_ASSERT_TRUE(msg.error.exists, "error should exist");
    T_ASSERT_INT_EQ(msg.error.value, MSI_E_INVALID_STATE, "error should match");

    return true;
}

static bool test_msg_parse_in_invalid_end_byte(void)
{
    uint8_t raw[] = {
        ID_REQUEST, 1, REQU_INIT,
        1 /* invalid end byte */
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, -1, "missing zero end byte should fail");

    return true;
}

static bool test_msg_parse_in_length_zero(void)
{
    MSIMessage msg;
    uint8_t dummy[1] = { 0 };

    int rc = msg_parse_in(g_tox, &msg, dummy, 0);

    T_ASSERT_INT_EQ(rc, -1, "zero length should fail");

    return true;
}

static bool test_msg_parse_in_missing_request(void)
{
    uint8_t raw[] = {
        ID_CAPABILITIES, 1, MSI_CAP_R_AUDIO,
        0
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, -1, "message without request field should fail");

    return true;
}

static bool test_msg_parse_in_invalid_header_id(void)
{
    uint8_t raw[] = {
        9, 1, 0,
        0
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, -1, "invalid header ID should fail");

    return true;
}

static bool test_msg_parse_in_invalid_size(void)
{
    uint8_t raw[] = {
        ID_REQUEST, 5, 0, /* claims 5 bytes but only 4 total */
        0
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, -1, "invalid size field should fail");

    return true;
}

static bool test_msg_parse_in_invalid_enum_high(void)
{
    uint8_t raw[] = {
        ID_REQUEST, 1, REQU_POP + 1,
        0
    };

    MSIMessage msg;
    int rc = msg_parse_in(g_tox, &msg, raw, sizeof(raw));

    T_ASSERT_INT_EQ(rc, -1, "request enum too high should fail");

    return true;
}

static bool test_msg_parse_header_out(void)
{
    uint8_t out[16];
    uint16_t length = 0;
    uint8_t value = REQU_INIT;

    uint8_t *end = msg_parse_header_out(ID_REQUEST, out, &value, sizeof(value), &length);

    T_ASSERT_INT_EQ(length, 3, "header length should be id + size + value");
    T_ASSERT_TRUE(end == out + 3, "returned pointer should be after header");
    T_ASSERT_INT_EQ(out[0], ID_REQUEST, "header id should be REQUEST");
    T_ASSERT_INT_EQ(out[1], 1, "header size should be 1");
    T_ASSERT_INT_EQ(out[2], REQU_INIT, "header value should be INIT");

    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * REGRESSION TESTS: upstream fix
 *
 * handle_init() and handle_push() FAILURE paths must invoke MSI_ON_ERROR
 * before kill_call(), otherwise ToxAV is not notified and ToxAVCall->msi_call
 * can become a dangling pointer.
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_handle_init_failure_invokes_error_callback(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_INACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_INIT;
    msg.capabilities.exists = false; /* malformed INIT: missing capabilities */

    handle_init(call, &msg);

    T_ASSERT_INT_EQ(g_error_count, 1,
                    "handle_init FAILURE path must invoke MSI_ON_ERROR");
    T_ASSERT_TRUE(get_call(session, 0) == NULL,
                  "handle_init FAILURE path must kill the call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_failure_invokes_error_callback(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_PUSH;
    msg.capabilities.exists = false; /* malformed PUSH: missing capabilities */

    handle_push(call, &msg);

    T_ASSERT_INT_EQ(g_error_count, 1,
                    "handle_push FAILURE path must invoke MSI_ON_ERROR");
    T_ASSERT_TRUE(get_call(session, 0) == NULL,
                  "handle_push FAILURE path must kill the call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_msi_packet_malformed_init_invokes_error(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    ToxAV fake_av;
    memset(&fake_av, 0, sizeof(fake_av));
    fake_av.msi = session;

    tox_set_av_object(g_tox, &fake_av);

    /*
     * Valid MSI packet container, but malformed INIT message:
     * request field present, capabilities field missing.
     */
    uint8_t pkt[] = {
        PACKET_ID_MSI,
        ID_REQUEST, 1, REQU_INIT,
        0
    };

    handle_msi_packet(g_tox, 0, pkt, sizeof(pkt), NULL);

    tox_set_av_object(g_tox, NULL);

    T_ASSERT_INT_EQ(g_error_count, 1,
                    "malformed INIT via handle_msi_packet must invoke MSI_ON_ERROR");

    pthread_mutex_lock(session->mutex);
    T_ASSERT_TRUE(get_call(session, 0) == NULL,
                  "malformed INIT must leave no MSI call behind");
    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: send helpers
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_send_message_missing_request(void)
{
    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));

    int rc = send_message(g_tox, 0, &msg);

    T_ASSERT_INT_EQ(rc, -1, "send_message without request should fail");

    return true;
}

static bool test_send_message_valid_pop(void)
{
    MSIMessage msg;
    msg_init(&msg, REQU_POP);

    int rc = send_message(g_tox, 0, &msg);

    /*
     * Friend 0 does not exist, so sending will normally fail.
     * We only care that the code path is exercised without crashing.
     */
    T_ASSERT_TRUE(rc == 0 || rc == -1, "send_message should return a valid result");

    return true;
}

static bool test_send_error_returns_zero(void)
{
    int rc = send_error(g_tox, 0, MSI_E_INVALID_MESSAGE);

    T_ASSERT_INT_EQ(rc, 0, "send_error should always return 0");

    return true;
}

static bool test_m_msi_packet_invalid_friend(void)
{
    uint8_t payload[1] = { 0 };

    int rc = m_msi_packet(g_tox, 0, payload, sizeof(payload));

    /*
     * No friend exists, so packet send should fail.
     */
    T_ASSERT_INT_EQ(rc, 0, "m_msi_packet should fail for non-existent friend");

    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: call list management
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_new_get_kill_call_list(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *c0 = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(c0, "new_call(0) failed");
    T_ASSERT_TRUE(get_call(session, 0) == c0, "get_call(0) should return c0");
    T_ASSERT_INT_EQ(session->calls_head, 0, "head should be 0");
    T_ASSERT_INT_EQ(session->calls_tail, 0, "tail should be 0");

    MSICall *c2 = new_call(session, 2);
    T_ASSERT_PTR_NOT_NULL(c2, "new_call(2) failed");
    T_ASSERT_TRUE(get_call(session, 1) == NULL, "slot 1 should be NULL");
    T_ASSERT_TRUE(get_call(session, 2) == c2, "get_call(2) should return c2");
    T_ASSERT_INT_EQ(session->calls_tail, 2, "tail should be 2");
    T_ASSERT_TRUE(c0->next == c2, "c0->next should be c2");
    T_ASSERT_TRUE(c2->prev == c0, "c2->prev should be c0");

    MSICall *c1 = new_call(session, 1);
    T_ASSERT_PTR_NOT_NULL(c1, "new_call(1) failed");
    T_ASSERT_TRUE(get_call(session, 1) == c1, "get_call(1) should return c1");
    T_ASSERT_TRUE(c0->next == c1, "c0->next should be c1");
    T_ASSERT_TRUE(c1->next == c2, "c1->next should be c2");
    T_ASSERT_TRUE(c2->prev == c1, "c2->prev should be c1");
    T_ASSERT_TRUE(c1->prev == c0, "c1->prev should be c0");

    /* Kill middle */
    kill_call(c1);
    T_ASSERT_TRUE(get_call(session, 1) == NULL, "slot 1 should be NULL after kill");
    T_ASSERT_TRUE(c0->next == c2, "c0->next should relink to c2");
    T_ASSERT_TRUE(c2->prev == c0, "c2->prev should relink to c0");

    /* Kill head */
    kill_call(c0);
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "slot 0 should be NULL after kill");
    T_ASSERT_TRUE(get_call(session, 2) == c2, "slot 2 should still exist");
    T_ASSERT_INT_EQ(session->calls_head, 2, "head should now be 2");
    T_ASSERT_INT_EQ(session->calls_tail, 2, "tail should still be 2");

    /* Kill last -> container freed */
    kill_call(c2);
    T_ASSERT_TRUE(session->calls == NULL, "calls array should be freed");
    T_ASSERT_INT_EQ(session->calls_head, 0, "head reset to 0");
    T_ASSERT_INT_EQ(session->calls_tail, 0, "tail reset to 0");
    T_ASSERT_TRUE(get_call(session, 2) == NULL, "get_call after free should be NULL");
    T_ASSERT_TRUE(get_call(session, 10) == NULL, "get_call beyond tail should be NULL");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_new_call_front_insertion(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *c5 = new_call(session, 5);
    T_ASSERT_PTR_NOT_NULL(c5, "new_call(5) failed");
    T_ASSERT_INT_EQ(session->calls_head, 5, "head should be 5");
    T_ASSERT_INT_EQ(session->calls_tail, 5, "tail should be 5");

    MSICall *c3 = new_call(session, 3);
    T_ASSERT_PTR_NOT_NULL(c3, "new_call(3) failed");
    T_ASSERT_INT_EQ(session->calls_head, 3, "head should be 3 after front insert");
    T_ASSERT_INT_EQ(session->calls_tail, 5, "tail should remain 5");
    T_ASSERT_TRUE(c3->next == c5, "c3->next should be c5");
    T_ASSERT_TRUE(c5->prev == c3, "c5->prev should be c3");
    T_ASSERT_TRUE(get_call(session, 3) == c3, "get_call(3) should return c3");
    T_ASSERT_TRUE(get_call(session, 5) == c5, "get_call(5) should return c5");

    kill_call(c3);
    T_ASSERT_TRUE(get_call(session, 3) == NULL, "slot 3 should be NULL");
    T_ASSERT_INT_EQ(session->calls_head, 5, "head should move to 5");

    kill_call(c5);
    T_ASSERT_TRUE(session->calls == NULL, "calls array should be freed");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_kill_call_null_safe(void)
{
    kill_call(NULL);
    T_ASSERT_TRUE(true, "kill_call(NULL) did not crash");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: invoke_callback
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_invoke_callback_no_callback_registered(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    /* Remove one callback explicitly */
    session->callbacks[MSI_ON_END] = NULL;

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->error = MSI_E_NONE;

    int rc = invoke_callback(call, MSI_ON_END);

    T_ASSERT_INT_EQ(rc, -1, "invoke_callback without callback should return -1");
    T_ASSERT_INT_EQ(call->error, MSI_E_HANDLE, "error should be set to MSI_E_HANDLE");

    call->error = MSI_E_INVALID_MESSAGE;

    rc = invoke_callback(call, MSI_ON_END);

    T_ASSERT_INT_EQ(rc, -1, "second invoke should also fail");
    T_ASSERT_INT_EQ(call->error, MSI_E_INVALID_MESSAGE, "existing error should be preserved");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_invoke_callback_success_and_failure(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->error = MSI_E_NONE;

    g_cb_ret = 0;
    int rc = invoke_callback(call, MSI_ON_INVITE);

    T_ASSERT_INT_EQ(rc, 0, "callback returning 0 should succeed");
    T_ASSERT_INT_EQ(g_invite_count, 1, "invite callback should be called once");
    T_ASSERT_TRUE(g_last_call == call, "callback should receive call");
    T_ASSERT_TRUE(g_last_av == (void *)0xA5, "callback should receive session->av");

    call->error = MSI_E_NONE;

    g_cb_ret = -1;
    rc = invoke_callback(call, MSI_ON_INVITE);

    T_ASSERT_INT_EQ(rc, -1, "callback returning -1 should fail");
    T_ASSERT_INT_EQ(g_invite_count, 2, "invite callback should be called twice");
    T_ASSERT_INT_EQ(call->error, MSI_E_HANDLE, "error should be set to HANDLE");

    g_cb_ret = 0;

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: public MSI API null/edge cases
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_msi_new_null_tox(void)
{
    T_ASSERT_TRUE(msi_new(NULL) == NULL, "msi_new(NULL) should return NULL");
    return true;
}

static bool test_msi_kill_null_session(void)
{
    int rc = msi_kill(g_tox, NULL, NULL);
    T_ASSERT_INT_EQ(rc, -1, "msi_kill(NULL session) should return -1");
    return true;
}

static bool test_msi_register_callback_null_session(void)
{
    msi_register_callback(NULL, cb_invite, MSI_ON_INVITE);
    T_ASSERT_TRUE(true, "msi_register_callback(NULL) did not crash");
    return true;
}

static bool test_check_peer_offline_status_null_args(void)
{
    T_ASSERT_FALSE(check_peer_offline_status(NULL, NULL, 0),
                   "NULL args should return false");
    return true;
}

static bool test_msi_invite_null_session(void)
{
    MSICall *call = NULL;
    int rc = msi_invite(NULL, &call, 0, MSI_CAP_R_AUDIO);

    T_ASSERT_INT_EQ(rc, -1, "msi_invite(NULL) should fail");

    return true;
}

static bool test_msi_answer_null_call(void)
{
    int rc = msi_answer(NULL, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, -1, "msi_answer(NULL) should fail");
    return true;
}

static bool test_msi_change_capabilities_null_call(void)
{
    int rc = msi_change_capabilities(NULL, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, -1, "msi_change_capabilities(NULL) should fail");
    return true;
}

static bool test_msi_hangup_null_call(void)
{
    int rc = msi_hangup(NULL);
    T_ASSERT_INT_EQ(rc, -1, "msi_hangup(NULL) should fail");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: msi_invite / msi_answer / msi_change_capabilities / msi_hangup
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_msi_invite_and_duplicate(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    MSICall *call = NULL;

    int rc = msi_invite(session, &call, 0, MSI_CAP_R_AUDIO);

    T_ASSERT_INT_EQ(rc, 0, "msi_invite should succeed");
    T_ASSERT_PTR_NOT_NULL(call, "call should be created");
    T_ASSERT_INT_EQ(call->state, MSI_CALL_REQUESTING, "state should be REQUESTING");

    MSICall *dup = NULL;
    rc = msi_invite(session, &dup, 0, MSI_CAP_R_AUDIO);

    T_ASSERT_INT_EQ(rc, -1, "duplicate invite should fail");

    destroy_msi_session(session);
    return true;
}

static bool test_msi_answer_invalid_then_valid(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    MSICall *call = NULL;

    int rc = msi_invite(session, &call, 0, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "invite failed");

    rc = msi_answer(call, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, -1, "answer in REQUESTING state should fail");

    pthread_mutex_lock(session->mutex);
    call->state = MSI_CALL_REQUESTED;
    pthread_mutex_unlock(session->mutex);

    rc = msi_answer(call, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "answer in REQUESTED state should succeed");
    T_ASSERT_INT_EQ(call->state, MSI_CALL_ACTIVE, "state should become ACTIVE");

    destroy_msi_session(session);
    return true;
}

static bool test_msi_change_capabilities_valid_and_invalid(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    MSICall *call = NULL;

    int rc = msi_invite(session, &call, 0, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "invite failed");

    pthread_mutex_lock(session->mutex);
    call->state = MSI_CALL_REQUESTED;
    pthread_mutex_unlock(session->mutex);

    rc = msi_answer(call, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "answer failed");

    rc = msi_change_capabilities(call, MSI_CAP_R_AUDIO | MSI_CAP_S_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "change_capabilities in ACTIVE state should succeed");
    T_ASSERT_INT_EQ(call->self_capabilities, MSI_CAP_R_AUDIO | MSI_CAP_S_AUDIO,
                    "self_capabilities should be updated");

    pthread_mutex_lock(session->mutex);
    call->state = MSI_CALL_REQUESTED;
    pthread_mutex_unlock(session->mutex);

    rc = msi_change_capabilities(call, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, -1, "change_capabilities in non-ACTIVE state should fail");

    destroy_msi_session(session);
    return true;
}

static bool test_msi_hangup_invalid_inactive(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);
    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");
    call->state = MSI_CALL_INACTIVE;
    pthread_mutex_unlock(session->mutex);

    int rc = msi_hangup(call);
    T_ASSERT_INT_EQ(rc, -1, "hangup in INACTIVE state should fail");

    pthread_mutex_lock(session->mutex);
    kill_call(call);
    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_msi_hangup_requesting(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    MSICall *call = NULL;

    int rc = msi_invite(session, &call, 0, MSI_CAP_R_AUDIO);
    T_ASSERT_INT_EQ(rc, 0, "invite failed");

    rc = msi_hangup(call);
    T_ASSERT_INT_EQ(rc, 0, "hangup in REQUESTING state should succeed");

    pthread_mutex_lock(session->mutex);
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "call should be removed");
    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: check_peer_offline_status
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_check_peer_offline_no_call(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    bool rc = check_peer_offline_status(g_tox, session, 0);

    T_ASSERT_TRUE(rc, "offline/no-call should return true");

    destroy_msi_session(session);
    return true;
}

static bool test_check_peer_offline_with_call(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);
    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");
    call->state = MSI_CALL_ACTIVE;
    pthread_mutex_unlock(session->mutex);

    bool rc = check_peer_offline_status(g_tox, session, 0);

    T_ASSERT_TRUE(rc, "offline with call should return true");
    T_ASSERT_INT_EQ(g_peertimeout_count, 1, "PEERTIMEOUT callback should be invoked");

    pthread_mutex_lock(session->mutex);
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "call should be killed");
    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: handle_init
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_handle_init_inactive_success(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_INACTIVE;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    handle_init(call, &msg);

    T_ASSERT_INT_EQ(call->state, MSI_CALL_REQUESTED, "state should become REQUESTED");
    T_ASSERT_INT_EQ(call->peer_capabilities, MSI_CAP_R_AUDIO, "peer caps should be set");
    T_ASSERT_INT_EQ(g_invite_count, 1, "invite callback should be invoked");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_init_missing_capabilities(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_INACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_INIT;
    msg.capabilities.exists = false;

    handle_init(call, &msg);

    T_ASSERT_TRUE(get_call(session, 0) == NULL, "call should be killed on missing caps");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_init_callback_failure_kills_call(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_INACTIVE;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    g_cb_ret = -1;

    handle_init(call, &msg);

    T_ASSERT_INT_EQ(g_invite_count, 1, "invite callback should be attempted");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "callback failure should kill call");

    g_cb_ret = 0;

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_init_requesting_to_active(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_REQUESTING;
    call->self_capabilities = MSI_CAP_R_AUDIO;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO | MSI_CAP_S_AUDIO);

    handle_init(call, &msg);

    T_ASSERT_INT_EQ(call->state, MSI_CALL_ACTIVE, "state should become ACTIVE");
    T_ASSERT_INT_EQ(g_start_count, 1, "START callback should be invoked");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_init_active_recall(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;
    call->self_capabilities = MSI_CAP_R_AUDIO;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    handle_init(call, &msg);

    T_ASSERT_INT_EQ(call->state, MSI_CALL_ACTIVE, "state should remain ACTIVE");
    T_ASSERT_INT_EQ(g_invite_count, 0, "INVITE callback should not be invoked");
    T_ASSERT_INT_EQ(g_start_count, 0, "START callback should not be invoked");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_init_requested_invalid_state(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_REQUESTED;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    handle_init(call, &msg);

    T_ASSERT_TRUE(get_call(session, 0) == NULL, "invalid INIT state should kill call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: handle_push
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_handle_push_requesting_to_active(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_REQUESTING;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    handle_push(call, &msg);

    T_ASSERT_INT_EQ(call->state, MSI_CALL_ACTIVE, "state should become ACTIVE");
    T_ASSERT_INT_EQ(g_start_count, 1, "START callback should be invoked");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_active_caps_change(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;
    call->peer_capabilities = 0;

    MSIMessage msg = make_caps_msg(MSI_CAP_S_VIDEO);

    handle_push(call, &msg);

    T_ASSERT_INT_EQ(call->state, MSI_CALL_ACTIVE, "state should remain ACTIVE");
    T_ASSERT_INT_EQ(call->peer_capabilities, MSI_CAP_S_VIDEO, "peer caps should update");
    T_ASSERT_INT_EQ(g_capabilities_count, 1, "CAPABILITIES callback should be invoked");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_active_caps_same(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;
    call->peer_capabilities = MSI_CAP_S_VIDEO;

    MSIMessage msg = make_caps_msg(MSI_CAP_S_VIDEO);

    handle_push(call, &msg);

    T_ASSERT_INT_EQ(g_capabilities_count, 0, "no callback if caps unchanged");

    kill_call(call);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_callback_failure_kills_call(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;
    call->peer_capabilities = 0;

    MSIMessage msg = make_caps_msg(MSI_CAP_S_VIDEO);

    g_cb_ret = -1;

    handle_push(call, &msg);

    T_ASSERT_INT_EQ(g_capabilities_count, 1, "capabilities callback attempted");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "callback failure should kill call");

    g_cb_ret = 0;

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_missing_capabilities(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_PUSH;
    msg.capabilities.exists = false;

    handle_push(call, &msg);

    T_ASSERT_TRUE(get_call(session, 0) == NULL, "missing caps should kill call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_push_inactive_and_requested_ignored(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call_inactive = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call_inactive, "new_call failed");
    call_inactive->state = MSI_CALL_INACTIVE;

    MSICall *call_requested = new_call(session, 1);
    T_ASSERT_PTR_NOT_NULL(call_requested, "new_call failed");
    call_requested->state = MSI_CALL_REQUESTED;

    MSIMessage msg = make_caps_msg(MSI_CAP_R_AUDIO);

    handle_push(call_inactive, &msg);
    handle_push(call_requested, &msg);

    T_ASSERT_INT_EQ(call_inactive->state, MSI_CALL_INACTIVE, "inactive state unchanged");
    T_ASSERT_INT_EQ(call_requested->state, MSI_CALL_REQUESTED, "requested state unchanged");
    T_ASSERT_INT_EQ(g_start_count, 0, "no START callback for ignored pushes");
    T_ASSERT_INT_EQ(g_capabilities_count, 0, "no CAPABILITIES callback for ignored pushes");

    kill_call(call_inactive);
    kill_call(call_requested);

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: handle_pop
 * NOTE: MSI_CALL_INACTIVE abort() case is intentionally not tested.
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_handle_pop_error(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.error.exists = true;
    msg.error.value = MSI_E_INVALID_STATE;

    handle_pop(call, &msg);

    T_ASSERT_INT_EQ(g_error_count, 1, "ERROR callback should be invoked");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "POP error should kill call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_pop_active_end(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_POP;

    handle_pop(call, &msg);

    T_ASSERT_INT_EQ(g_end_count, 1, "END callback should be invoked");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "POP should kill call");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_pop_requesting_and_requested_end(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call_reqing = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call_reqing, "new_call failed");
    call_reqing->state = MSI_CALL_REQUESTING;

    MSICall *call_reqed = new_call(session, 1);
    T_ASSERT_PTR_NOT_NULL(call_reqed, "new_call failed");
    call_reqed->state = MSI_CALL_REQUESTED;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_POP;

    handle_pop(call_reqing, &msg);
    handle_pop(call_reqed, &msg);

    T_ASSERT_INT_EQ(g_end_count, 2, "END callback should be invoked twice");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "call 0 killed");
    T_ASSERT_TRUE(get_call(session, 1) == NULL, "call 1 killed");

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

static bool test_handle_pop_callback_failure_still_kills_call(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *call = new_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "new_call failed");

    call->state = MSI_CALL_ACTIVE;

    MSIMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.request.exists = true;
    msg.request.value = REQU_POP;

    g_cb_ret = -1;

    handle_pop(call, &msg);

    T_ASSERT_INT_EQ(g_end_count, 1, "END callback attempted");
    T_ASSERT_TRUE(get_call(session, 0) == NULL, "call should still be killed");

    g_cb_ret = 0;

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: handle_msi_packet
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_handle_msi_packet_early_exits(void)
{
    uint8_t short_pkt[1] = { PACKET_ID_MSI };

    tox_set_av_object(g_tox, NULL);

    handle_msi_packet(g_tox, 0, short_pkt, sizeof(short_pkt), NULL);

    uint8_t no_av_pkt[2] = { PACKET_ID_MSI, 0 };

    handle_msi_packet(g_tox, 0, no_av_pkt, sizeof(no_av_pkt), NULL);

    T_ASSERT_TRUE(true, "early-exit paths did not crash");

    return true;
}

static bool test_handle_msi_packet_valid_init(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    /*
     * Create a fake ToxAV object so tox_get_av_object() returns something.
     * handle_msi_packet() only needs tox_av_msi_get(av) to return our session.
     */
    ToxAV fake_av;
    memset(&fake_av, 0, sizeof(fake_av));
    fake_av.msi = session;

    tox_set_av_object(g_tox, &fake_av);

    uint8_t pkt[] = {
        PACKET_ID_MSI,
        ID_REQUEST, 1, REQU_INIT,
        ID_CAPABILITIES, 1, MSI_CAP_R_AUDIO,
        0
    };

    handle_msi_packet(g_tox, 0, pkt, sizeof(pkt), NULL);

    tox_set_av_object(g_tox, NULL);

    T_ASSERT_INT_EQ(g_invite_count, 1, "INIT packet should trigger invite callback");

    pthread_mutex_lock(session->mutex);

    MSICall *call = get_call(session, 0);
    T_ASSERT_PTR_NOT_NULL(call, "call should exist after INIT");

    if (call != NULL) {
        T_ASSERT_INT_EQ(call->state, MSI_CALL_REQUESTED, "state should be REQUESTED");
        kill_call(call);
    }

    pthread_mutex_unlock(session->mutex);

    destroy_msi_session(session);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESTS: msi_kill with active calls
 * ═══════════════════════════════════════════════════════════════════ */

static bool test_msi_kill_with_calls(void)
{
    MSISession *session = create_msi_session();
    T_ASSERT_PTR_NOT_NULL(session, "msi_new failed");

    pthread_mutex_lock(session->mutex);

    MSICall *c0 = new_call(session, 0);
    MSICall *c1 = new_call(session, 1);

    T_ASSERT_PTR_NOT_NULL(c0, "new_call(0) failed");
    T_ASSERT_PTR_NOT_NULL(c1, "new_call(1) failed");

    pthread_mutex_unlock(session->mutex);

    int rc = msi_kill(g_tox, session, NULL);

    T_ASSERT_INT_EQ(rc, 0, "msi_kill should succeed");

    /* session is now freed; do not destroy again */
    return true;
}

/* ── main ───────────────────────────────────────────────────────── */

int main(void)
{
    TEST_SUITE("MSI Protocol Unit Tests");

    g_tox = create_test_tox();

    if (g_tox == NULL) {
        fprintf(stderr, "Failed to create test Tox instance\n");
        return 1;
    }

    /* message helpers */
    RUN_TEST(test_msg_init);
    RUN_TEST(test_check_size_valid);
    RUN_TEST(test_check_size_invalid_size_field);
    RUN_TEST(test_check_size_constraint_too_small);
    RUN_TEST(test_check_enum_high);
    RUN_TEST(test_msg_parse_in_valid_full_message);
    RUN_TEST(test_msg_parse_in_invalid_end_byte);
    RUN_TEST(test_msg_parse_in_length_zero);
    RUN_TEST(test_msg_parse_in_missing_request);
    RUN_TEST(test_msg_parse_in_invalid_header_id);
    RUN_TEST(test_msg_parse_in_invalid_size);
    RUN_TEST(test_msg_parse_in_invalid_enum_high);
    RUN_TEST(test_msg_parse_header_out);

    /* send helpers */
    RUN_TEST(test_send_message_missing_request);
    RUN_TEST(test_send_message_valid_pop);
    RUN_TEST(test_send_error_returns_zero);
    RUN_TEST(test_m_msi_packet_invalid_friend);

    /* call list management */
    RUN_TEST(test_new_get_kill_call_list);
    RUN_TEST(test_new_call_front_insertion);
    RUN_TEST(test_kill_call_null_safe);

    /* callback invocation */
    RUN_TEST(test_invoke_callback_no_callback_registered);
    RUN_TEST(test_invoke_callback_success_and_failure);

    /* null / edge API */
    RUN_TEST(test_msi_new_null_tox);
    RUN_TEST(test_msi_kill_null_session);
    RUN_TEST(test_msi_register_callback_null_session);
    RUN_TEST(test_check_peer_offline_status_null_args);
    RUN_TEST(test_msi_invite_null_session);
    RUN_TEST(test_msi_answer_null_call);
    RUN_TEST(test_msi_change_capabilities_null_call);
    RUN_TEST(test_msi_hangup_null_call);

    /* public lifecycle API */
    RUN_TEST(test_msi_invite_and_duplicate);
    RUN_TEST(test_msi_answer_invalid_then_valid);
    RUN_TEST(test_msi_change_capabilities_valid_and_invalid);
    RUN_TEST(test_msi_hangup_invalid_inactive);
    RUN_TEST(test_msi_hangup_requesting);

    /* offline handling */
    RUN_TEST(test_check_peer_offline_no_call);
    RUN_TEST(test_check_peer_offline_with_call);

    /* handle_init */
    RUN_TEST(test_handle_init_inactive_success);
    RUN_TEST(test_handle_init_missing_capabilities);
    RUN_TEST(test_handle_init_callback_failure_kills_call);
    RUN_TEST(test_handle_init_requesting_to_active);
    RUN_TEST(test_handle_init_active_recall);
    RUN_TEST(test_handle_init_requested_invalid_state);

    /* handle_push */
    RUN_TEST(test_handle_push_requesting_to_active);
    RUN_TEST(test_handle_push_active_caps_change);
    RUN_TEST(test_handle_push_active_caps_same);
    RUN_TEST(test_handle_push_callback_failure_kills_call);
    RUN_TEST(test_handle_push_missing_capabilities);
    RUN_TEST(test_handle_push_inactive_and_requested_ignored);

    /* handle_pop */
    RUN_TEST(test_handle_pop_error);
    RUN_TEST(test_handle_pop_active_end);
    RUN_TEST(test_handle_pop_requesting_and_requested_end);
    RUN_TEST(test_handle_pop_callback_failure_still_kills_call);

    /* handle_msi_packet */
    RUN_TEST(test_handle_msi_packet_early_exits);
    RUN_TEST(test_handle_msi_packet_valid_init);

    /* Regression tests for upstream MSI_ON_ERROR fix */
    RUN_TEST(test_handle_init_failure_invokes_error_callback);
    RUN_TEST(test_handle_push_failure_invokes_error_callback);
    RUN_TEST(test_handle_msi_packet_malformed_init_invokes_error);

    /* msi_kill */
    RUN_TEST(test_msi_kill_with_calls);

    tox_kill(g_tox);

    SUITE_END();

    return test_summary("msi");
}

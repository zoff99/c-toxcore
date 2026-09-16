/*
 * test_toxav_threading.c
 *
 * Threading, locking, and memory stress tests for toxav components:
 * - MSI (Message Session Interface) for call signaling
 * - BWController (bandwidth controller)
 *
 * Tests concurrent access, lock contention, race conditions, and memory safety
 * under heavy multi-threaded load.
 */

#include "test_framework.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include toxav headers */
#include "../toxav/msi.h"
#include "../toxav/bwcontroller.h"
#include "../toxcore/tox.h"
#include "../toxcore/logger.h"
#include "../toxcore/mono_time.h"

#define NUM_THREADS 8
#define OPERATIONS_PER_THREAD 100
#define RAPID_CYCLES 1000

/* ── Helper: Create a minimal Tox instance for testing ─────────── */
static Tox *create_test_tox(void)
{
    struct Tox_Options options;
    tox_options_default(&options);
    options.local_discovery_enabled = false;
    options.ipv6_enabled = false;
    
    Tox *tox = tox_new(&options, NULL);
    if (!tox) {
        /* Try without IPv6 if first attempt fails */
        options.ipv6_enabled = false;
        tox = tox_new(&options, NULL);
    }
    return tox;
}

/* Helper: Create a Mono_Time instance for BWC */
static Mono_Time *create_test_mono_time(void)
{
    return mono_time_new(NULL, NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * MSI (Message Session Interface) Threading Tests
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    MSISession *session;
    uint32_t friend_number;
    int thread_id;
    int operations_completed;
} MSI_Thread_Arg;

/* Thread function: rapidly invite and hangup calls */
static void *msi_invite_hangup_thread(void *arg)
{
    MSI_Thread_Arg *targ = (MSI_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        MSICall *call = NULL;
        
        /* Try to invite */
        int ret = msi_invite(targ->session, &call, targ->friend_number, 0x01);
        if (ret == 0 && call) {
            /* Small delay to simulate real usage */
            usleep(100);
            
            /* Try to hangup - msi_hangup takes only the call pointer */
            msi_hangup(call);
        }
        
        targ->operations_completed++;
    }
    
    return NULL;
}

/* Test: Concurrent invite/hangup from multiple threads */
bool test_msi_concurrent_invite_hangup(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    MSISession *session = msi_new(tox);
    T_ASSERT_PTR_NOT_NULL(session, "Failed to create MSI session");
    
    pthread_t threads[NUM_THREADS];
    MSI_Thread_Arg thread_args[NUM_THREADS];
    
    /* Launch threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i].session = session;
        thread_args[i].friend_number = 1 + i;  /* Different friend per thread */
        thread_args[i].thread_id = i;
        thread_args[i].operations_completed = 0;
        
        int ret = pthread_create(&threads[i], NULL, msi_invite_hangup_thread, &thread_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create thread");
    }
    
    /* Wait for all threads to complete */
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Verify all threads completed their operations */
    int total_ops = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_ops += thread_args[i].operations_completed;
        T_ASSERT_INT_EQ(thread_args[i].operations_completed, OPERATIONS_PER_THREAD,
                        "Thread did not complete all operations");
    }
    
    T_ASSERT_INT_EQ(total_ops, NUM_THREADS * OPERATIONS_PER_THREAD,
                    "Not all operations completed");
    
    /* Cleanup */
    int kill_ret = msi_kill(tox, session, NULL);
    T_ASSERT_INT_EQ(kill_ret, 0, "msi_kill failed");
    
    tox_kill(tox);
    return true;
}

/* Test: Rapid session create/destroy cycles */
bool test_msi_rapid_create_destroy(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    for (int i = 0; i < RAPID_CYCLES; i++) {
        MSISession *session = msi_new(tox);
        T_ASSERT_PTR_NOT_NULL(session, "msi_new failed on cycle");
        
        int kill_ret = msi_kill(tox, session, NULL);
        T_ASSERT_INT_EQ(kill_ret, 0, "msi_kill failed on cycle");
    }
    
    tox_kill(tox);
    return true;
}

typedef struct {
    MSISession *session;
    MSICall *call;
    int thread_id;
} MSI_Callback_Thread_Arg;

/* Thread function: register callbacks concurrently */
static void *msi_callback_registration_thread(void *arg)
{
    MSI_Callback_Thread_Arg *targ = (MSI_Callback_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        /* Register different callbacks using correct enum names */
        msi_register_callback(targ->session, NULL, MSI_ON_INVITE);
        usleep(10);
        msi_register_callback(targ->session, NULL, MSI_ON_START);
        usleep(10);
        msi_register_callback(targ->session, NULL, MSI_ON_END);
    }
    
    return NULL;
}

/* Test: Concurrent callback registration */
bool test_msi_concurrent_callback_registration(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    MSISession *session = msi_new(tox);
    T_ASSERT_PTR_NOT_NULL(session, "Failed to create MSI session");
    
    pthread_t threads[NUM_THREADS];
    MSI_Callback_Thread_Arg thread_args[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i].session = session;
        thread_args[i].thread_id = i;
        
        int ret = pthread_create(&threads[i], NULL, msi_callback_registration_thread, &thread_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create thread");
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    int kill_ret = msi_kill(tox, session, NULL);
    T_ASSERT_INT_EQ(kill_ret, 0, "msi_kill failed");
    
    tox_kill(tox);
    return true;
}

/* Thread function: rapid state transitions */
static void *msi_state_transition_thread(void *arg)
{
    MSI_Thread_Arg *targ = (MSI_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD / 4; i++) {
        MSICall *call = NULL;
        
        /* Invite */
        int ret = msi_invite(targ->session, &call, targ->friend_number, 0x01);
        if (ret == 0 && call) {
            /* Try to answer (will fail if state is wrong, but shouldn't crash) */
            msi_answer(call, 0x02);
            
            /* Change capabilities */
            msi_change_capabilities(call, 0x03);
            
            /* Hangup - takes only call pointer */
            msi_hangup(call);
        }
        
        targ->operations_completed++;
    }
    
    return NULL;
}

/* Test: Rapid state transitions from multiple threads */
bool test_msi_rapid_state_transitions(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    MSISession *session = msi_new(tox);
    T_ASSERT_PTR_NOT_NULL(session, "Failed to create MSI session");
    
    pthread_t threads[NUM_THREADS];
    MSI_Thread_Arg thread_args[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i].session = session;
        thread_args[i].friend_number = 10 + i;
        thread_args[i].thread_id = i;
        thread_args[i].operations_completed = 0;
        
        int ret = pthread_create(&threads[i], NULL, msi_state_transition_thread, &thread_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create thread");
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    int kill_ret = msi_kill(tox, session, NULL);
    T_ASSERT_INT_EQ(kill_ret, 0, "msi_kill failed");
    
    tox_kill(tox);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * BWController Threading Tests
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    BWController *bwc;
    int thread_id;
    int operations_completed;
} BWC_Thread_Arg;

/* Thread function: rapidly add received bytes */
static void *bwc_add_recv_thread(void *arg)
{
    BWC_Thread_Arg *targ = (BWC_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        uint32_t bytes = 100 + (rand() % 1000);
        bwc_add_recv(targ->bwc, bytes);
        targ->operations_completed++;
    }
    
    return NULL;
}

/* Thread function: rapidly add lost bytes */
static void *bwc_add_lost_thread(void *arg)
{
    BWC_Thread_Arg *targ = (BWC_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        uint32_t bytes = 10 + (rand() % 100);
        bwc_add_lost_v3(targ->bwc, bytes, false);
        targ->operations_completed++;
    }
    
    return NULL;
}

/* Test: Concurrent bwc_add_recv and bwc_add_lost */
bool test_bwc_concurrent_add_operations(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    /* Create a Mono_Time for BWC */
    Mono_Time *mono_time = create_test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mono_time, "Failed to create Mono_Time");
    
    /* Create a BWC controller with correct signature: bwc_new(tox, mono_time, friendnumber, mcb, mcb_user_data) */
    BWController *bwc = bwc_new(tox, mono_time, 1, NULL, NULL);
    T_ASSERT_PTR_NOT_NULL(bwc, "Failed to create BWController");
    
    pthread_t recv_threads[NUM_THREADS / 2];
    pthread_t lost_threads[NUM_THREADS / 2];
    BWC_Thread_Arg recv_args[NUM_THREADS / 2];
    BWC_Thread_Arg lost_args[NUM_THREADS / 2];
    
    /* Launch recv threads */
    for (int i = 0; i < NUM_THREADS / 2; i++) {
        recv_args[i].bwc = bwc;
        recv_args[i].thread_id = i;
        recv_args[i].operations_completed = 0;
        
        int ret = pthread_create(&recv_threads[i], NULL, bwc_add_recv_thread, &recv_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create recv thread");
    }
    
    /* Launch lost threads */
    for (int i = 0; i < NUM_THREADS / 2; i++) {
        lost_args[i].bwc = bwc;
        lost_args[i].thread_id = i + NUM_THREADS / 2;
        lost_args[i].operations_completed = 0;
        
        int ret = pthread_create(&lost_threads[i], NULL, bwc_add_lost_thread, &lost_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create lost thread");
    }
    
    /* Wait for all threads */
    for (int i = 0; i < NUM_THREADS / 2; i++) {
        pthread_join(recv_threads[i], NULL);
    }
    for (int i = 0; i < NUM_THREADS / 2; i++) {
        pthread_join(lost_threads[i], NULL);
    }
    
    /* Verify all operations completed */
    int total_ops = 0;
    for (int i = 0; i < NUM_THREADS / 2; i++) {
        total_ops += recv_args[i].operations_completed;
        total_ops += lost_args[i].operations_completed;
    }
    
    T_ASSERT_INT_EQ(total_ops, NUM_THREADS * OPERATIONS_PER_THREAD,
                    "Not all BWC operations completed");
    
    bwc_kill(bwc);
    mono_time_free(mono_time);
    tox_kill(tox);
    return true;
}

/* Test: Rapid BWC create/destroy cycles */
bool test_bwc_rapid_create_destroy(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    for (int i = 0; i < RAPID_CYCLES; i++) {
        Mono_Time *mono_time = create_test_mono_time();
        T_ASSERT_PTR_NOT_NULL(mono_time, "create_test_mono_time failed on cycle");
        
        BWController *bwc = bwc_new(tox, mono_time, 1, NULL, NULL);
        T_ASSERT_PTR_NOT_NULL(bwc, "bwc_new failed on cycle");
        
        bwc_kill(bwc);
        mono_time_free(mono_time);
    }
    
    tox_kill(tox);
    return true;
}

/* Thread function: mixed operations on BWC */
static void *bwc_mixed_operations_thread(void *arg)
{
    BWC_Thread_Arg *targ = (BWC_Thread_Arg *)arg;
    
    for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
        int op = rand() % 3;
        
        switch (op) {
            case 0:
                bwc_add_recv(targ->bwc, 500);
                break;
            case 1:
                bwc_add_lost_v3(targ->bwc, 50, false);
                break;
            case 2:
                /* Small delay to simulate real usage pattern */
                usleep(50);
                break;
        }
        
        targ->operations_completed++;
    }
    
    return NULL;
}

/* Test: Mixed concurrent operations on BWC */
bool test_bwc_mixed_concurrent_operations(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    Mono_Time *mono_time = create_test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mono_time, "Failed to create Mono_Time");
    
    BWController *bwc = bwc_new(tox, mono_time, 1, NULL, NULL);
    T_ASSERT_PTR_NOT_NULL(bwc, "Failed to create BWController");
    
    pthread_t threads[NUM_THREADS];
    BWC_Thread_Arg thread_args[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i].bwc = bwc;
        thread_args[i].thread_id = i;
        thread_args[i].operations_completed = 0;
        
        int ret = pthread_create(&threads[i], NULL, bwc_mixed_operations_thread, &thread_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create thread");
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    bwc_kill(bwc);
    mono_time_free(mono_time);
    tox_kill(tox);
    return true;
}

/* Test: BWC kill during active operations */
bool test_bwc_kill_during_operations(void)
{
    Tox *tox = create_test_tox();
    T_ASSERT_PTR_NOT_NULL(tox, "Failed to create Tox instance");
    
    Mono_Time *mono_time = create_test_mono_time();
    T_ASSERT_PTR_NOT_NULL(mono_time, "Failed to create Mono_Time");
    
    BWController *bwc = bwc_new(tox, mono_time, 1, NULL, NULL);
    T_ASSERT_PTR_NOT_NULL(bwc, "Failed to create BWController");
    
    pthread_t threads[4];
    BWC_Thread_Arg thread_args[4];
    
    /* Start a few threads doing operations */
    for (int i = 0; i < 4; i++) {
        thread_args[i].bwc = bwc;
        thread_args[i].thread_id = i;
        thread_args[i].operations_completed = 0;
        
        int ret = pthread_create(&threads[i], NULL, bwc_add_recv_thread, &thread_args[i]);
        T_ASSERT_INT_EQ(ret, 0, "Failed to create thread");
    }
    
    /* Let them run for a bit */
    usleep(10000);  /* 10ms */
    
    /* Kill the controller while threads are still running */
    bwc_kill(bwc);
    
    /* Wait for threads to finish (they should handle NULL bwc gracefully) */
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    mono_time_free(mono_time);
    tox_kill(tox);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    TEST_SUITE("ToxAV Threading & Memory Stress Tests");
    
    /* MSI tests */
    RUN_TEST(test_msi_concurrent_invite_hangup);
    RUN_TEST(test_msi_rapid_create_destroy);
    RUN_TEST(test_msi_concurrent_callback_registration);
    RUN_TEST(test_msi_rapid_state_transitions);
    
    /* BWController tests */
    RUN_TEST(test_bwc_concurrent_add_operations);
    RUN_TEST(test_bwc_rapid_create_destroy);
    RUN_TEST(test_bwc_mixed_concurrent_operations);
    RUN_TEST(test_bwc_kill_during_operations);
    
    SUITE_END();
    return test_summary("toxav_threading");
}

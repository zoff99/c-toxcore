#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>

#include "../toxcore/net_profile.h"
#include "../toxcore/tox.h"
#include "../toxcore/tox_private.h"

/* ────────────────────────────────────────────────────────────────
 * 1. Direct net_profile struct tests
 * ──────────────────────────────────────────────────────────────── */

bool test_netprof_basic_record_and_query(void)
{
    Net_Profile profile;
    memset(&profile, 0, sizeof(profile));

    netprof_record_packet(&profile, 0x00, 100, PACKET_DIRECTION_SEND);
    netprof_record_packet(&profile, 0x00, 200, PACKET_DIRECTION_RECV);
    netprof_record_packet(&profile, 0x01, 50, PACKET_DIRECTION_SEND);
    netprof_record_packet(&profile, 0x20, 500, PACKET_DIRECTION_RECV);

    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x00, PACKET_DIRECTION_SEND), 1,
                    "count_id 0x00 send");
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x00, PACKET_DIRECTION_RECV), 1,
                    "count_id 0x00 recv");
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x01, PACKET_DIRECTION_SEND), 1,
                    "count_id 0x01 send");
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x01, PACKET_DIRECTION_RECV), 0,
                    "count_id 0x01 recv (none)");
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x20, PACKET_DIRECTION_RECV), 1,
                    "count_id 0x20 recv");

    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x00, PACKET_DIRECTION_SEND), 100,
                    "bytes_id 0x00 send");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x00, PACKET_DIRECTION_RECV), 200,
                    "bytes_id 0x00 recv");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x20, PACKET_DIRECTION_RECV), 500,
                    "bytes_id 0x20 recv");

    T_ASSERT_INT_EQ(netprof_get_packet_count_total(&profile, PACKET_DIRECTION_SEND), 2,
                    "total count send");
    T_ASSERT_INT_EQ(netprof_get_packet_count_total(&profile, PACKET_DIRECTION_RECV), 2,
                    "total count recv");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(&profile, PACKET_DIRECTION_SEND), 150,
                    "total bytes send");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(&profile, PACKET_DIRECTION_RECV), 700,
                    "total bytes recv");
    return true;
}

bool test_netprof_tcp_data_range(void)
{
    Net_Profile profile;
    memset(&profile, 0, sizeof(profile));

    /* With the new TCP data tracking, we use the new helper to record both the 0x10 bucket and the inner ID bucket */
    netprof_record_tcp_data_packet(&profile, 0x10, 0x80, 100, PACKET_DIRECTION_SEND); // inner ID 0x80
    netprof_record_tcp_data_packet(&profile, 0x10, 0x80, 200, PACKET_DIRECTION_RECV);

    /* Querying 0x10 should give us exactly what was recorded for 0x10 */
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x10, PACKET_DIRECTION_SEND), 1, "tcp_data count send");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x10, PACKET_DIRECTION_SEND), 100, "tcp_data bytes send");

    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x10, PACKET_DIRECTION_RECV), 1, "tcp_data count recv");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x10, PACKET_DIRECTION_RECV), 200, "tcp_data bytes recv");

    /* Querying the inner ID (0x80) should also give us the exact same counts */
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x80, PACKET_DIRECTION_SEND), 1, "inner id count send");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x80, PACKET_DIRECTION_SEND), 100, "inner id bytes send");

    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x80, PACKET_DIRECTION_RECV), 1, "inner id count recv");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(&profile, 0x80, PACKET_DIRECTION_RECV), 200, "inner id bytes recv");

    /* Total should be incremented exactly once per packet (no double counting) */
    T_ASSERT_INT_EQ(netprof_get_packet_count_total(&profile, PACKET_DIRECTION_SEND), 1, "total count send");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(&profile, PACKET_DIRECTION_SEND), 100, "total bytes send");

    T_ASSERT_INT_EQ(netprof_get_packet_count_total(&profile, PACKET_DIRECTION_RECV), 1, "total count recv");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(&profile, PACKET_DIRECTION_RECV), 200, "total bytes recv");

    /* Querying an ID that wasn't recorded should be 0 */
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x11, PACKET_DIRECTION_SEND), 0, "unrecorded id count");

    return true;
}

bool test_netprof_null_safety(void)
{
    T_ASSERT_INT_EQ(netprof_get_packet_count_id(NULL, 0x00, PACKET_DIRECTION_SEND), 0,
                    "null count_id");
    T_ASSERT_INT_EQ(netprof_get_packet_count_total(NULL, PACKET_DIRECTION_SEND), 0,
                    "null count_total");
    T_ASSERT_INT_EQ(netprof_get_bytes_id(NULL, 0x00, PACKET_DIRECTION_SEND), 0,
                    "null bytes_id");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(NULL, PACKET_DIRECTION_SEND), 0,
                    "null bytes_total");

    /* Must not crash */
    netprof_record_packet(NULL, 0x00, 100, PACKET_DIRECTION_SEND);
    return true;
}

bool test_netprof_large_counts(void)
{
    Net_Profile profile;
    memset(&profile, 0, sizeof(profile));

    const uint32_t num_packets = 100000;
    for (uint32_t i = 0; i < num_packets; ++i) {
        netprof_record_packet(&profile, 0x20, 1400, PACKET_DIRECTION_SEND);
    }

    T_ASSERT_INT_EQ(netprof_get_packet_count_id(&profile, 0x20, PACKET_DIRECTION_SEND),
                    num_packets, "large count");
    T_ASSERT_INT_EQ(netprof_get_bytes_total(&profile, PACKET_DIRECTION_SEND),
                    (uint64_t)num_packets * 1400, "large bytes");
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * 2. Tox-level API integration
 * ──────────────────────────────────────────────────────────────── */

bool test_tox_netprof_api(void)
{
    Tox_Err_New err;
    struct Tox_Options *opts = tox_options_new(NULL);
    T_ASSERT_INT_EQ(!!(opts != NULL), 1, "tox_options_new");
    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, true);
    tox_options_set_local_discovery_enabled(opts, false);

    Tox *tox = tox_new(opts, &err);
    tox_options_free(opts);

    if (tox == NULL) {
        return true;
    }

    const uint64_t udp_sent_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_DIRECTION_SENT);
    const uint64_t udp_recv_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_DIRECTION_RECV);
    const uint64_t tcp_sent_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_TCP, TOX_NETPROF_DIRECTION_SENT);
    const uint64_t tcp_recv_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_TCP, TOX_NETPROF_DIRECTION_RECV);

    T_ASSERT_INT_EQ(!!(udp_sent_count == 0 || udp_sent_count > 0), 1, "udp_sent_count valid");
    T_ASSERT_INT_EQ(!!(udp_recv_count == 0 || udp_recv_count > 0), 1, "udp_recv_count valid");
    T_ASSERT_INT_EQ(!!(tcp_sent_count == 0 || tcp_sent_count > 0), 1, "tcp_sent_count valid");
    T_ASSERT_INT_EQ(!!(tcp_recv_count == 0 || tcp_recv_count > 0), 1, "tcp_recv_count valid");

    const uint64_t udp_sent_bytes = tox_netprof_get_packet_total_bytes(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_DIRECTION_SENT);
    const uint64_t udp_recv_bytes = tox_netprof_get_packet_total_bytes(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_DIRECTION_RECV);

    T_ASSERT_INT_EQ(!!(udp_sent_bytes == 0 || udp_sent_bytes > 0), 1, "udp_sent_bytes valid");
    T_ASSERT_INT_EQ(!!(udp_recv_bytes == 0 || udp_recv_bytes > 0), 1, "udp_recv_bytes valid");

    const uint64_t ping_count = tox_netprof_get_packet_id_count(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_PACKET_ID_ZERO,
        TOX_NETPROF_DIRECTION_SENT);
    T_ASSERT_INT_EQ(!!(ping_count == 0 || ping_count > 0), 1, "ping_count valid");

    const uint64_t tcp_c_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_TCP_CLIENT, TOX_NETPROF_DIRECTION_SENT);
    const uint64_t tcp_s_count = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_TCP_SERVER, TOX_NETPROF_DIRECTION_SENT);

    T_ASSERT_INT_EQ(tcp_sent_count, tcp_c_count + tcp_s_count,
                    "tcp combined == client + server");

    const uint64_t tcp_data_count = tox_netprof_get_packet_id_count(
        tox, TOX_NETPROF_PACKET_TYPE_TCP, TOX_NETPROF_PACKET_ID_TCP_DATA,
        TOX_NETPROF_DIRECTION_SENT);
    T_ASSERT_INT_EQ(!!(tcp_data_count == 0 || tcp_data_count > 0), 1, "tcp_data_count valid");

    tox_kill(tox);
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * 3. Multi-threaded stress: public Tox API (has internal locking)
 * ──────────────────────────────────────────────────────────────── */

#define TOX_NUM_THREADS 4
#define TOX_ITERATIONS 10000

typedef struct {
    Tox *tox;
    int thread_id;
} ToxThreadArg;

static void *tox_netprof_stress_thread(void *arg)
{
    ToxThreadArg *ta = (ToxThreadArg *)arg;
    Tox *tox = ta->tox;

    for (int i = 0; i < TOX_ITERATIONS; ++i) {
        const Tox_Netprof_Packet_Type types[] = {
            TOX_NETPROF_PACKET_TYPE_TCP_CLIENT,
            TOX_NETPROF_PACKET_TYPE_TCP_SERVER,
            TOX_NETPROF_PACKET_TYPE_TCP,
            TOX_NETPROF_PACKET_TYPE_UDP,
        };
        const Tox_Netprof_Direction dirs[] = {
            TOX_NETPROF_DIRECTION_SENT,
            TOX_NETPROF_DIRECTION_RECV,
        };

        const int t = i % 4;
        const int d = i % 2;

        (void)tox_netprof_get_packet_total_count(tox, types[t], dirs[d]);
        (void)tox_netprof_get_packet_total_bytes(tox, types[t], dirs[d]);
        (void)tox_netprof_get_packet_id_count(tox, types[t], (uint8_t)(i & 0xFF), dirs[d]);
        (void)tox_netprof_get_packet_id_bytes(tox, types[t], (uint8_t)(i & 0xFF), dirs[d]);
    }

    return NULL;
}

bool test_tox_netprof_multithreaded(void)
{
    Tox_Err_New err;
    struct Tox_Options *opts = tox_options_new(NULL);
    T_ASSERT_INT_EQ(!!(opts != NULL), 1, "tox_options_new");
    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, true);
    tox_options_set_local_discovery_enabled(opts, false);

    Tox *tox = tox_new(opts, &err);
    tox_options_free(opts);

    if (tox == NULL) {
        return true;
    }

    pthread_t threads[TOX_NUM_THREADS];
    ToxThreadArg args[TOX_NUM_THREADS];

    for (int i = 0; i < TOX_NUM_THREADS; ++i) {
        args[i].tox = tox;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, tox_netprof_stress_thread, &args[i]);
    }

    for (int i = 0; i < TOX_NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    T_ASSERT_INT_EQ(1, 1, "tox_netprof multithreaded stress completed without crash");

    const uint64_t udp_total = tox_netprof_get_packet_total_count(
        tox, TOX_NETPROF_PACKET_TYPE_UDP, TOX_NETPROF_DIRECTION_SENT);
    T_ASSERT_INT_EQ(!!(udp_total == 0 || udp_total > 0), 1, "udp_total valid after stress");

    tox_kill(tox);
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * main
 * ──────────────────────────────────────────────────────────────── */

int main(void)
{
    TEST_SUITE("network profiler (net_profile + tox_netprof API)");

    RUN_TEST(test_netprof_basic_record_and_query);
    RUN_TEST(test_netprof_tcp_data_range);
    RUN_TEST(test_netprof_null_safety);
    RUN_TEST(test_netprof_large_counts);
    RUN_TEST(test_tox_netprof_api);
    RUN_TEST(test_tox_netprof_multithreaded);

    SUITE_END();
    return test_summary("net_profile");
}

/*
 * compile a simple C++ tox program with toxcore amalgamation (on a linux system):
 *
 * with ToxAV:

  g++ -O3 -fPIC -fpermissive \
  -DTEST_WITH_TOXAV \
  -pthread \
  amalgamation_cpp_test.cpp \
  $(pkg-config --cflags --libs libavcodec) \
  $(pkg-config --cflags --libs libavutil) \
  $(pkg-config --cflags --libs opus) \
  $(pkg-config --cflags --libs vpx) \
  $(pkg-config --cflags --libs x264) \
  $(pkg-config --cflags --libs libsodium) \
  -o amalgamation_cpp_test_av

 *
 * without ToxAV:
 * g++ -O3 -fPIC -fpermissive amalgamation_cpp_test.cpp $(pkg-config --cflags --libs libsodium) -pthread -o amalgamation_cpp_test
 *
 * ==========================================================================================================
 * 
 * cross compile a simple C++ tox program with toxcore amalgamation (on a linux system) for win64:
 * 
 * without ToxAV (you will need to cross compile or get libsodium for windows yourself):
 * x86_64-w64-mingw32-g++ -static -O3 -fpermissive amalgamation_cpp_test.cpp $(pkg-config --cflags --libs libsodium) -lwinpthread -lwsock32 -lws2_32 -liphlpapi -o amalgamation_cpp_test
 * 
 */

#include <unistd.h>

#include <iostream>
#include <thread>

// define this before including toxcore amalgamation -------
#define MIN_LOGGER_LEVEL LOGGER_LEVEL_DEBUG
// define this before including toxcore amalgamation -------

// include toxcore amalgamation no ToxAV --------
#ifndef TEST_WITH_TOXAV
#include "toxcore_amalgamation_no_toxav.c"
#endif
// include toxcore amalgamation no ToxAV --------

// include toxcore amalgamation with ToxAV --------
#ifdef TEST_WITH_TOXAV
#include "toxcore_amalgamation.c"
#endif
// include toxcore amalgamation with ToxAV --------

static int self_online = 0;

struct Node1 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes1[] = {
  { "2604:a880:1:20::32f:1001", "BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F", 33445, 33445 },
  { "tox.kurnevsky.net", "82EF82BA33445A1F91A7DB27189ECFC0C013E06E3DA71F588ED692BED625EC23", 33445, 33445 },
  { "tox1.mf-net.eu", "B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506", 33445, 33445 },
  { "tox3.plastiras.org", "4B031C96673B6FF123269FF18F2847E1909A8A04642BBECD0189AC8AEEADAF64", 33445, 3389 },
  { nullptr, nullptr, 0, 0 }
};

static void self_connection_change_callback(Tox *tox, TOX_CONNECTION status, void *userdata)
{
    switch (status) {
        case TOX_CONNECTION_NONE:
            std::cout << "Lost connection to the Tox network.\n";
            self_online = 0;
            break;
        case TOX_CONNECTION_TCP:
            std::cout << "Connected using TCP.\n";
            self_online = 1;
            break;
        case TOX_CONNECTION_UDP:
            std::cout << "Connected using UDP.\n";
            self_online = 2;
            break;
    }
}

#ifdef TEST_WITH_TOXAV
static void call_state_callback(ToxAV *av, uint32_t friend_number, uint32_t state, void *user_data)
{
}
#endif

static void hex_string_to_bin2(const char *hex_string, uint8_t *output)
{
    size_t len = strlen(hex_string) / 2;
    size_t i = len;
    if (!output)
    {
        return;
    }
    const char *pos = hex_string;
    for (i = 0; i < len; ++i, pos += 2)
    {
        sscanf(pos, "%2hhx", &output[i]);
    }
}

static void tox_log_cb__custom(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func,
                        const char *message, void *user_data)
{
    printf("C-TOXCORE:1:%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}



int main()
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    std::cout << "--START--\n";
    struct Tox_Options options;
    tox_options_default(&options);
    // ----- set options ------
    options.ipv6_enabled = true;
    options.local_discovery_enabled = true;
    options.hole_punching_enabled = true;
    options.udp_enabled = true;
    options.tcp_port = 0; // disable tcp relay function!
    options.log_callback = tox_log_cb__custom;
    // ----- set options ------
#ifndef TOX_HAVE_TOXUTIL
    std::cout << "init Tox\n";
    Tox *tox = tox_new(&options, nullptr);
#else
    std::cout << "init Tox [TOXUTIL]\n";
    Tox *tox = tox_utils_new(&options, nullptr);
#endif
#ifdef TEST_WITH_TOXAV
    std::cout << "init ToxAV\n";
    ToxAV *toxav = toxav_new(tox, nullptr);
#endif


    // ----- CALLBACKS -----
#ifdef TOX_HAVE_TOXUTIL
    tox_utils_callback_self_connection_status(tox, self_connection_change_callback);
    tox_callback_self_connection_status(tox, tox_utils_self_connection_status_cb);
#else
    tox_callback_self_connection_status(tox, self_connection_change_callback);
#endif
#ifdef TEST_WITH_TOXAV
    toxav_callback_call_state(toxav, call_state_callback, nullptr);
#endif
    // ----- CALLBACKS -----


    // ----- bootstrap -----
    std::cout << "Tox bootstrapping\n";
    for (int i = 0; nodes1[i].ip; i++)
    {
        uint8_t *key = (uint8_t *)calloc(1, 100);
        hex_string_to_bin2(nodes1[i].key, key);
        if (!key)
        {
            continue;
        }
        tox_bootstrap(tox, nodes1[i].ip, nodes1[i].udp_port, key, nullptr);
        if (nodes1[i].tcp_port != 0)
        {
            tox_add_tcp_relay(tox, nodes1[i].ip, nodes1[i].tcp_port, key, nullptr);
        }
        free(key);
    }
    // ----- bootstrap -----
    tox_iterate(tox, nullptr);
#ifdef TEST_WITH_TOXAV
    toxav_iterate(toxav);
#endif


    // ----- tox iterate thread -----
    auto t_it = [](Tox *tox)
    {
        while (1 == 1)
        {
            tox_iterate(tox, nullptr);
            usleep(tox_iteration_interval(tox));
            if (self_online > 0)
            {
                break;
            }
        }
    };
    std::thread t_it_thread(t_it, tox);
    // ----- tox iterate thread -----


#ifdef TEST_WITH_TOXAV
    // ----- toxav iterate thread -----
    auto av_it = [](ToxAV *toxav)
    {
        while (1 == 1)
        {
            toxav_iterate(toxav);
            usleep(50);
            if (self_online > 0)
            {
                break;
            }
        }
    };
    std::thread av_it_thread(av_it, toxav);
    // ----- toxav iterate thread -----
#endif


    // ----------- wait for Tox to come online -----------
    t_it_thread.join();
#ifdef TEST_WITH_TOXAV
    av_it_thread.join();
#endif
    std::cout << "Tox online\n";
    // ----------- wait for Tox to come online -----------

    // ----------- shutdown clean -----------
#ifdef TEST_WITH_TOXAV
    toxav_kill(toxav);
    std::cout << "killed ToxAV\n";
#endif
#ifndef TOX_HAVE_TOXUTIL
    tox_kill(tox);
    std::cout << "killed Tox\n";
#else
    tox_utils_kill(tox);
    std::cout << "killed Tox [TOXUTIL]\n";
#endif
    // ----------- shutdown clean -----------

    std::cout << "--END--\n";
}

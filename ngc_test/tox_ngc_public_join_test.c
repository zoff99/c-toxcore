/*
 * tox_ngc_public_join_test.c
 *
 * Minimal test: tox1 creates a public NGC group, tox2 joins via Chat ID.
 * No friend relationship. TCP-only.
 *
 * Compile:
 *   gcc -g -o tox_ngc_public_join_test tox_ngc_public_join_test.c ../amalgamation/libtoxcore.a -lsodium -lopus -lx265 -lx264 -lavcodec -lvpx -lavutil
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#include "../toxcore/tox.h"

static volatile sig_atomic_t running = 1;
static void signal_handler(int signum) { running = 0; }

/* ---------- Bootstrap nodes ---------- */
typedef struct {
    const char *host;
    uint16_t    port;
    const char *public_key_hex;
} Bootstrap_Node;

static const Bootstrap_Node BOOTSTRAP_NODES[] = {
    { "tox.novg.net",        33445, "D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463" },
    { "tox.initramfs.io",    33445, "3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25" },
    { "tox.kurnevsky.net",   33445, "82EF82BA33445A1F91A7DB27189ECFC0C013E8E8A64D829A508B21C0250B0139" },
    { "144.217.167.73",   33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "tox.abilinski.com",   33445, "10C00EB250C3233E343E2AEBA07115A5C28920E9C8D29492F6D00B29049EDC7E" },
    { "tox1.mf-net.eu",   33445, "B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506" },
    { "3.0.24.15",   33445, "E20ABCF38CDBFFD7D04B29C956B33F7B27A3BB7AF0618101617B036E4AEA402D" },
};
#define BOOTSTRAP_COUNT (sizeof(BOOTSTRAP_NODES) / sizeof(BOOTSTRAP_NODES[0]))

/* ---------- Helpers ---------- */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_hex_key(const char *hex, uint8_t *out) {
    if (!hex || !out) return false;
    size_t len = strlen(hex);
    if (len != TOX_PUBLIC_KEY_SIZE * 2) return false;
    for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; i++) {
        int h = hex_digit(hex[i*2]), l = hex_digit(hex[i*2+1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}

static void log_msg(const char *who, const char *fmt, ...) {
    char buf[64];
    time_t now = time(NULL);
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&now));
    printf("[%s][%s] ", buf, who);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
}

static void bootstrap(Tox *tox) {
    for (size_t i = 0; i < BOOTSTRAP_COUNT; i++) {
        uint8_t pk[TOX_PUBLIC_KEY_SIZE];
        if (!parse_hex_key(BOOTSTRAP_NODES[i].public_key_hex, pk)) continue;
        Tox_Err_Bootstrap e1, e2;
        tox_add_tcp_relay(tox, BOOTSTRAP_NODES[i].host, BOOTSTRAP_NODES[i].port, pk, &e1);
        tox_bootstrap(tox, BOOTSTRAP_NODES[i].host, BOOTSTRAP_NODES[i].port, pk, &e2);
    }
}

/* ---------- Callback state ---------- */
typedef struct {
    const char *name;
    bool        net_connected;
    uint32_t    group_number;
    bool        group_connected;
} ClientState;

void tox_log_cb__custom(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func,
                        const char *message, void *ud)
{
    ClientState *cs = ud;
    // printf("[%s] C-TOXCORE:%d:%s:%d:%s:%s\n", cs->name, (int)level, file, (int)line, func, message);
    fflush(stdout);
}

static Tox *create_tox(ClientState *cs) {
    Tox_Err_Options_New oerr;
    struct Tox_Options *opts = tox_options_new(&oerr);
    if (!opts) exit(1);

    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, false);           /* TCP only */
    tox_options_set_local_discovery_enabled(opts, false);
    // tox_options_set_dht_announcements_enabled(opts, true);
    tox_options_set_hole_punching_enabled(opts, false);
    // tox_options_set_start_port(opts, 0);
    // tox_options_set_end_port(opts, 0);
    // tox_options_set_tcp_port(opts, 0);

#if 1
    tox_options_set_proxy_type(opts, TOX_PROXY_TYPE_SOCKS5);
    tox_options_set_proxy_host(opts, "localhost");
    tox_options_set_proxy_port(opts, 9050);
#endif

    tox_options_set_log_user_data(opts, cs);
    tox_options_set_log_callback(opts, tox_log_cb__custom);

    Tox_Err_New err;
    Tox *tox = tox_new(opts, &err);
    tox_options_free(opts);
    if (!tox) { fprintf(stderr, "tox_new failed: %d\n", err); exit(1); }
    return tox;
}

/* ---------- Callbacks ---------- */
static void cb_self_conn(Tox *tox, Tox_Connection status, void *ud) {
    ClientState *cs = ud;
    bool conn = (status != TOX_CONNECTION_NONE);
    if (cs->net_connected != conn)
        log_msg(cs->name, "Network: %s", conn ? "TCP" : "NONE");
    cs->net_connected = conn;
}

static void cb_group_self_join(Tox *tox, uint32_t gn, void *ud) {
    ClientState *cs = ud;
    cs->group_number = gn;
    cs->group_connected = true;
    log_msg(cs->name, "Self joined group %u", gn);
}

static void cb_group_conn_status(Tox *tox, uint32_t gn, int32_t status, void *ud) {
    ClientState *cs = ud;
    if (cs->group_number == UINT32_MAX) cs->group_number = gn;
    cs->group_connected = (status == 3);
    log_msg(cs->name, "Group %u status=%d", gn, (int)status);
}

static void cb_group_peer_join(Tox *tox, uint32_t gn, uint32_t peer_id, void *ud) {
    ClientState *cs = ud;
    log_msg(cs->name, "Peer %u joined group %u", peer_id, gn);
}

static void cb_group_peer_exit(Tox *tox, uint32_t gn, uint32_t peer_id,
                                Tox_Group_Exit_Type type,
                                const uint8_t *name, size_t name_len,
                                const uint8_t *part_msg, size_t part_len, void *ud) {
    ClientState *cs = ud;
    log_msg(cs->name, "Peer %u exited group %u (type=%d)", peer_id, gn, (int)type);
}

static void cb_group_join_fail(Tox *tox, uint32_t gn, Tox_Group_Join_Fail type, void *ud) {
    ClientState *cs = ud;
    log_msg(cs->name, "Join failed group %u reason=%d", gn, (int)type);
}

static void register_callbacks(Tox *tox, ClientState *cs) {
    tox_callback_self_connection_status(tox, cb_self_conn);
    tox_callback_group_self_join(tox, cb_group_self_join);
    tox_callback_group_connection_status(tox, cb_group_conn_status);
    tox_callback_group_peer_join(tox, cb_group_peer_join);
    tox_callback_group_peer_exit(tox, cb_group_peer_exit);
    tox_callback_group_join_fail(tox, cb_group_join_fail);
}

/* ---------- Main ---------- */
int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* --- Create both clients --- */
    ClientState tox1_state = { .name = "tox1", .group_number = UINT32_MAX };
    ClientState tox2_state = { .name = "tox2", .group_number = UINT32_MAX };

    Tox *tox1 = create_tox(&tox1_state);
    Tox *tox2 = create_tox(&tox2_state);
    register_callbacks(tox1, &tox1_state);
    register_callbacks(tox2, &tox2_state);

    log_msg("main", "Bootstrapping both clients (TCP-only)...");
    bootstrap(tox1);
    bootstrap(tox2);

    /* --- Wait for network connectivity --- */
    log_msg("main", "Waiting for network connectivity...");
    time_t wait_start = time(NULL);
    while (running && (time(NULL) - wait_start < 120)) {
        tox_iterate(tox1, &tox1_state);
        tox_iterate(tox2, &tox2_state);
        if (tox1_state.net_connected && tox2_state.net_connected) break;
        usleep(50000);
    }
    if (!tox1_state.net_connected || !tox2_state.net_connected) {
        log_msg("main", "Timeout waiting for network. Exiting.");
        goto cleanup;
    }
    log_msg("main", "Both clients connected to network.");


#if 0
    log_msg("main", "Both clients connected. Waiting 60s for TCP relay state to settle...");
    time_t settle_start = time(NULL);
    while (running && time(NULL) - settle_start < 60) {
        tox_iterate(tox1, &tox1_state);
        tox_iterate(tox2, &tox2_state);
        usleep(50000);
    }
    log_msg("tox1", "Creating public NGC group after relay settle wait...");
#endif

    /* --- Phase 1: tox1 creates a PUBLIC group --- */
    log_msg("tox1", "Creating public NGC group...");
    Tox_Err_Group_New gerr;
    uint32_t gn = tox_group_new(tox1, TOX_GROUP_PRIVACY_STATE_PUBLIC,
                                (const uint8_t *)"PublicTest", 10,
                                (const uint8_t *)"tox1", 4, &gerr);
    if (gerr != TOX_ERR_GROUP_NEW_OK) {
        log_msg("tox1", "Failed to create group: %d", (int)gerr);
        goto cleanup;
    }
    tox1_state.group_number = gn;
    tox1_state.group_connected = true;
    log_msg("tox1", "Created public group %u", gn);

    /* Get the Chat ID so tox2 can join */
    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    Tox_Err_Group_State_Queries qerr;
    if (!tox_group_get_chat_id(tox1, gn, chat_id, &qerr) || qerr != TOX_ERR_GROUP_STATE_QUERIES_OK) {
        log_msg("tox1", "Failed to get chat ID");
        goto cleanup;
    }

    char chat_id_hex[TOX_GROUP_CHAT_ID_SIZE * 2 + 1];
    for (size_t i = 0; i < TOX_GROUP_CHAT_ID_SIZE; i++)
        snprintf(chat_id_hex + i*2, 3, "%02X", chat_id[i]);
    chat_id_hex[TOX_GROUP_CHAT_ID_SIZE * 2] = '\0';
    log_msg("tox1", "Chat ID: %s", chat_id_hex);

    /* --- Wait before tox2 joins --- */
    log_msg("main", "Waiting 10 seconds before tox2 joins...");
    time_t phase_start = time(NULL);
    while (running && (time(NULL) - phase_start < 10)) {
        tox_iterate(tox1, &tox1_state);
        tox_iterate(tox2, &tox2_state);
        usleep(50000);
    }

    /* --- Phase 2: tox2 joins the public group via Chat ID --- */
    log_msg("tox2", "Joining public group via Chat ID...");
    Tox_Err_Group_Join jerr;
    uint32_t gn2 = tox_group_join(tox2, chat_id,
                                  (const uint8_t *)"tox2", 4,
                                  NULL, 0, &jerr);
    if (jerr != TOX_ERR_GROUP_JOIN_OK) {
        log_msg("tox2", "Failed to join group: %d", (int)jerr);
        goto cleanup;
    }
    tox2_state.group_number = gn2;
    log_msg("tox2", "Join initiated, local group number: %u", gn2);

    /* --- Run until both are connected or timeout --- */
    log_msg("main", "Waiting for both clients to be fully connected...");
    time_t run_start = time(NULL);
    bool success = false;

    while (running && (time(NULL) - run_start < 300)) {
        tox_iterate(tox1, &tox1_state);
        tox_iterate(tox2, &tox2_state);

        if (tox1_state.group_connected && tox2_state.group_connected) {
            if (!success) {
                success = true;
                log_msg("main", "SUCCESS: Both clients connected to the public group!");
                log_msg("tox1", "Group %u connected", tox1_state.group_number);
                log_msg("tox2", "Group %u connected", tox2_state.group_number);
            }
            /* Keep iterating for a bit to let sync complete */
            if (time(NULL) - run_start > 60) break;
        }

        /* Re-bootstrap periodically if disconnected */
        if (!tox1_state.net_connected || !tox2_state.net_connected) {
            static time_t last_bs = 0;
            time_t now = time(NULL);
            if (now - last_bs >= 15) {
                bootstrap(tox1);
                bootstrap(tox2);
                last_bs = now;
            }
        }

        usleep(50000);
    }

    if (!success) {
        log_msg("main", "TIMEOUT: Clients did not fully connect within 300s");
    }

cleanup:
    log_msg("main", "Cleaning up...");
    if (tox1) tox_kill(tox1);
    if (tox2) tox_kill(tox2);
    log_msg("main", "Done.");
    return success ? 0 : 1;
}

/*

tox_ngc_tcp_test.c

Test client with hidden-state mid_roster middleware integration:
3 Tox instances in one process
UDP disabled, TCP-only
Bootstrap to 3 external bootstrap nodes
tox1 friends tox2 and tox3
tox1 creates a private NGC group
tox1 invites tox2 and tox3
tox2/tox3 auto-accept the invite
Middleware automatically announces presence, requests roster, and verifies signatures
tox3 is simulated to disconnect, tox4 joins and receives the persistent roster
tox3 reconnects and the group fully syncs

Compile with:

gcc -fsanitize=address -g -fno-omit-frame-pointer tox_ngc_tcp_test.c mid_roster.c -I. ../amalgamation/libtoxcore.a -lsodium -lopus -lx265 -lx264 -lavcodec -lvpx -lavutil

*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "../toxcore/tox.h"
#include "mid_roster.h"

#define NUM_CLIENTS 3
#define BOOTSTRAP_NODE_COUNT 3

static volatile sig_atomic_t running = 1;

typedef struct {
    const char *host;
    uint16_t    port;
    const char *public_key_hex;
} Bootstrap_Node;

static const Bootstrap_Node BOOTSTRAP_NODES[BOOTSTRAP_NODE_COUNT] = {
    {
        .host = "tox.novg.net",
        .port = 33445,
        .public_key_hex = "D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463"
    },
    {
        .host = "tox.initramfs.io",
        .port = 33445,
        .public_key_hex = "3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25"
    },
    {
        .host = "tox.kurnevsky.net",
        .port = 33445,
        .public_key_hex = "82EF82BA33445A1F91A7DB27189ECFC0C013E8E8A64D829A508B21C0250B0139"
    },
};

typedef struct {
    int          index;
    const char  *name;
    Tox         *tox;
    MidState    *mid;  /* Opaque middleware state. Handles all groups internally. */

    uint8_t      address[TOX_ADDRESS_SIZE];
    bool         network_connected;
    time_t       last_bootstrap;

    uint32_t     group_number;
    bool         group_connected;

    uint32_t     friend_num[NUM_CLIENTS];
    bool         friend_online[NUM_CLIENTS];
    bool         invited[NUM_CLIENTS];
} Client;

static Client clients[NUM_CLIENTS];

/* --- PHASE 2+ INTEGRATION --- */
static int test_phase = 1;
static bool tox3_exited_group = false;
static Client tox4_client;
static bool tox4_initialized = false;
/* ---------------------------- */

/* --- PHASE 7 INTEGRATION --- */
#define PHASE7_TOMBSTONE_FLUSH_SEC 5
#define PHASE7_SYNC_WAIT_SEC       10
#define PHASE7_TIMEOUT_SEC         180

static int      phase7_step = 0;
static time_t   phase7_step_time = 0;
static time_t   phase7_sync_since = 0;

static bool     tox2_identity_known = false;
static uint8_t  tox2_identity[TOX_GROUP_PEER_PUBLIC_KEY_SIZE];
static uint32_t tox2_old_group_number = UINT32_MAX;
static bool     tox2_tombstone_sent = false;
static bool     tox2_group_left = false;
/* ---------------------------- */

/* --- PHASE 8 INTEGRATION --- */
static uint8_t tox4_identity[TOX_GROUP_PEER_PUBLIC_KEY_SIZE];
static bool tox4_identity_known = false;
static bool tox4_kicked = false;
static bool tox4_cleanup_done = false;
static int phase8_step = 0;
static time_t phase8_step_time = 0;
static time_t phase8_sync_since = 0;
static time_t phase8_last_kick_attempt = 0;
/* ---------------------------- */

/* --- PHASE 9 INTEGRATION --- */
/*
 * A string containing deliberately broken UTF-8 and invalid bytes.
 * \xC0\xAF is an overlong encoding.
 * \xFE\xFF are never valid in UTF-8.
 * \x80\x81 are unexpected continuation bytes.
 * This tests that the middleware treats nicknames as opaque binary
 * and does not crash or corrupt memory when handling them.
 */
static const char tox5_broken_name[] = "tox5_\xC0\xAF\xFE\xFF\x80\x81";
static Client tox5_client;
static bool tox5_initialized = false;
/* ---------------------------- */

/******************************************************************************
Logging helpers
******************************************************************************/

static void log_client(const Client *client, const char *fmt, ...)
{
    char timebuf[64];
    time_t now = time(NULL);
    struct tm *tm_buf = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_buf);

    printf("[%s][%s] ", timebuf, client ? client->name : "?");

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

static void log_raw(const char *fmt, ...)
{
    char timebuf[64];
    time_t now = time(NULL);
    struct tm *tm_buf = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_buf);

    printf("[%s][main] ", timebuf);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

/******************************************************************************
Hex key parsing & Bootstrap
******************************************************************************/

static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_hex_public_key(const char *hex, uint8_t *public_key)
{
    if (hex == NULL || public_key == NULL) return false;

    size_t len = strlen(hex);
    if (len != TOX_PUBLIC_KEY_SIZE * 2) return false;

    for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; i++) {
        int high = hex_digit_value(hex[i * 2]);
        int low  = hex_digit_value(hex[i * 2 + 1]);

        if (high < 0 || low < 0) return false;

        public_key[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

static void bootstrap_client_to_network(Client *client)
{
    client->last_bootstrap = time(NULL);

    log_client(client, "Bootstrapping to %d external bootstrap nodes", BOOTSTRAP_NODE_COUNT);

    for (size_t i = 0; i < BOOTSTRAP_NODE_COUNT; i++) {
        const Bootstrap_Node *node = &BOOTSTRAP_NODES[i];

        uint8_t public_key[TOX_PUBLIC_KEY_SIZE];
        if (!parse_hex_public_key(node->public_key_hex, public_key)) continue;

        Tox_Err_Bootstrap tcp_err;
        tox_add_tcp_relay(client->tox, node->host, node->port, public_key, &tcp_err);

        Tox_Err_Bootstrap boot_err;
        tox_bootstrap(client->tox, node->host, node->port, public_key, &boot_err);
    }
}

/******************************************************************************
Helper lookups
******************************************************************************/

static bool get_client_chat_id(Client *client, uint8_t *chat_id)
{
    if (client == NULL || client->tox == NULL || client->group_number == UINT32_MAX || chat_id == NULL) return false;
    Tox_Err_Group_State_Queries err;
    bool ok = tox_group_get_chat_id(client->tox, client->group_number, chat_id, &err);
    return ok && err == TOX_ERR_GROUP_STATE_QUERIES_OK;
}

static bool has_tombstone(Client *c, const uint8_t *id) {
    if (c->group_number == UINT32_MAX) return false;
    uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
    if (!get_client_chat_id(c, cid)) return false;
    return mid_peer_is_signed_left(c->mid, cid, id);
}

static bool peer_absent(Client *c, const uint8_t *id) {
    if (c->group_number == UINT32_MAX) return true;
    uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
    if (!get_client_chat_id(c, cid)) return false;
    return mid_find_peer(c->mid, cid, id) == -1;
}

static int find_client_index_by_public_key(const uint8_t *public_key)
{
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (memcmp(clients[i].address, public_key, TOX_PUBLIC_KEY_SIZE) == 0) return i;
    }

    if (tox4_initialized && memcmp(tox4_client.address, public_key, TOX_PUBLIC_KEY_SIZE) == 0) {
        return 3;
    }

    if (tox5_initialized && memcmp(tox5_client.address, public_key, TOX_PUBLIC_KEY_SIZE) == 0) {
        return 4;
    }

    return -1;
}

static int find_friend_index(const Client *client, uint32_t friend_number)
{
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (client->friend_num[i] == friend_number) return i;
    }

    return -1;
}

static bool client_is_group_connected(Client *client)
{
    if (client->tox == NULL || client->group_number == UINT32_MAX) return false;

    Tox_Err_Group_Is_Connected err;
    int32_t status = tox_group_is_connected(client->tox, client->group_number, &err);

    if (err != TOX_ERR_GROUP_IS_CONNECTED_OK) return false;

    client->group_connected = (status == 1);
    return client->group_connected;
}

/******************************************************************************
Peer table printing using the public mid_peer_list API

This replaces mid_print_peer_table() with a client-side implementation
that uses only mid_peer_list_count() and mid_peer_list_get().
This demonstrates how a real client or JNI wrapper would render the list.
******************************************************************************/

static void print_peer_table(const MidState *mid, const uint8_t *chat_id, const char *title)
{
    time_t now = time(NULL);
    size_t count = mid_peer_list_count((MidState *)mid, chat_id);

    uint64_t sent = 0;
    uint64_t recv_bytes = 0;
    mid_get_network_stats((MidState *)mid, &sent, &recv_bytes);

    char chat_id_hex[9];
    for(int i=0; i<4; i++) snprintf(chat_id_hex + i*2, 3, "%02X", chat_id[i]);
    chat_id_hex[8] = '\0';

    printf("\n");
    printf("==========================================================================================\n");
    printf("PEER TABLE: %s (ChatID %s...)\n", title != NULL ? title : "unknown", chat_id_hex);
    printf("peers=%zu sent=%llu recv=%llu\n", count, (unsigned long long)sent, (unsigned long long)recv_bytes);
    printf("+----+--------------+--------------+--------------+--------+------+--------+----------+-------+----------+\n");
    printf("| %2s | %-12s | %-12s | %-12s | %-6s | %-4s | %-6s | %-8s | %-8s | %-8s |\n",
           "#", "Nickname", "Identity", "Signing", "Status", "Sig", "Conn", "Role", "Last seen", "NickLen");
    printf("+----+--------------+--------------+--------------+--------+------+--------+----------+-------+----------+\n");

    for (size_t i = 0; i < count; i++) {
        MidPeerInfo info;

        if (!mid_peer_list_get((MidState *)mid, chat_id, i, &info)) {
            printf("| %2zu | (failed to read peer)                                                                        |\n", i);
            continue;
        }

        /*
         * Format identity key (first 6 bytes as hex).
         */
        char identity_buf[13];
        char signing_buf[13];
        identity_buf[0] = '\0';
        signing_buf[0] = '\0';

        for (int j = 0; j < 6; j++) {
            snprintf(identity_buf + (j * 2), 3, "%02X", info.identity_key[j]);
            snprintf(signing_buf + (j * 2), 3, "%02X", info.signing_key[j]);
        }
        identity_buf[12] = '\0';
        signing_buf[12] = '\0';

        const char *conn_str = "NONE";
        if (info.connection_status == TOX_CONNECTION_TCP) conn_str = "TCP";
        else if (info.connection_status == TOX_CONNECTION_UDP) conn_str = "UDP";

        const char *role_str = "USER";
        if (info.role == TOX_GROUP_ROLE_FOUNDER) role_str = "FOUNDER";
        else if (info.role == TOX_GROUP_ROLE_MODERATOR) role_str = "MOD";
        else if (info.role == TOX_GROUP_ROLE_OBSERVER) role_str = "OBS";

        /*
         * Format last seen.
         */
        char seen_buf[16];
        if (info.connection_status != TOX_CONNECTION_NONE) {
            snprintf(seen_buf, sizeof(seen_buf), "now");
        } else if (info.last_seen == 0) {
            snprintf(seen_buf, sizeof(seen_buf), "-");
        } else if (info.last_seen > (uint64_t)now) {
            snprintf(seen_buf, sizeof(seen_buf), "future");
        } else {
            unsigned long age = (unsigned long)(now - (time_t)info.last_seen);
            snprintf(seen_buf, sizeof(seen_buf), "%lus", age);
        }

        /*
         * Format status.
         */
        const char *status_str = "?";
        if (info.status == 0) status_str = "ACTIVE";
        else if (info.status == 1) status_str = "LEFT";

        /*
         * Sanitize nickname for display (printable ASCII only).
         */
        char nick_buf[13];
        size_t display_len = info.nickname_len;
        if (display_len > 12) display_len = 12;
        for (size_t j = 0; j < display_len; j++) {
            uint8_t c = (uint8_t)info.nickname[j];
            nick_buf[j] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
        }
        nick_buf[display_len] = '\0';

        printf("| %2zu | %-12s | %-12s | %-12s | %-6s | %-4s | %-6s | %-8s | %-8s | %-8u |\n",
               i,
               nick_buf,
               identity_buf,
               signing_buf,
               status_str,
               info.has_signature ? "yes" : "no",
               conn_str,
               role_str,
               seen_buf,
               (unsigned)info.nickname_len);
    }

    printf("+----+--------------+--------------+--------------+--------+------+--------+----------+-------+----------+\n");
    printf("\n");

    fflush(stdout);
}

static void print_client_peer_table(Client *c, const char *title) {
    if (c->group_number == UINT32_MAX) return;
    uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
    if (get_client_chat_id(c, cid)) {
        print_peer_table(c->mid, cid, title);
    }
}

/******************************************************************************
Group leave / delete helper

Wraps tox_group_leave() + mid_on_group_delete() + local state reset so
every leave/delete path goes through one place. This ensures the
middleware state is always cleaned up when we leave or delete a group.
******************************************************************************/

static void client_leave_group(Client *client)
{
    if (client == NULL || client->group_number == UINT32_MAX) {
        return;
    }

    uint32_t gn = client->group_number;

    log_client(client, "Leaving group %u", gn);

    Tox_Err_Group_Leave lerr;
    tox_group_leave(client->tox, gn, NULL, 0, &lerr);

    /*
     * Always clean up middleware state regardless of whether
     * tox_group_leave succeeded. The group may already be gone
     * from Toxcore's perspective (e.g. after a kick).
     */
    mid_on_group_delete(client->mid, client->tox, gn);

    client->group_number = UINT32_MAX;
    client->group_connected = false;

    log_client(client, "Group %u left and middleware state removed", gn);
}

/******************************************************************************
Peer-list-changed callback (demonstrates the new mid API)

This is called by the middleware whenever the roster for a group changes.
In a real client you would call mid_peer_list_count() and mid_peer_list_get()
here to refresh your UI list.
******************************************************************************/

static void peer_list_changed_cb(const uint8_t *chat_id, void *user_data)
{
    Client *client = user_data;
    if (client == NULL) return;

    size_t count = mid_peer_list_count(client->mid, chat_id);

    char chat_id_hex[9];
    for(int i=0; i<4; i++) snprintf(chat_id_hex + i*2, 3, "%02X", chat_id[i]);
    chat_id_hex[8] = '\0';

    log_client(client,
               "Peer list changed for chat_id %s... (%zu peers in middleware roster)",
               chat_id_hex,
               count);

    /*
     * Example: iterate the full peer list using the flat getter API.
     * This is the same pattern a JNI wrapper would use.
     */
    for (size_t i = 0; i < count; i++) {
        MidPeerInfo info;
        if (mid_peer_list_get(client->mid, chat_id, i, &info)) {
            /*
             * In a real client, you would push each MidPeerInfo into
             * your UI model here. For this test we just log a summary.
             */
            log_client(client,
                       "  peer[%zu]: status=%u conn=%u signed=%u nick_len=%u",
                       i,
                       info.status,
                       info.connection_status,
                       info.has_signature,
                       info.nickname_len);
        }
    }
}

/******************************************************************************
Group invite logic for tox1
******************************************************************************/

static void invite_if_ready(Client *client, int friend_index)
{
    if (client->index != 0) return;
    if (friend_index <= 0 || friend_index >= NUM_CLIENTS) return;
    if (client->group_number == UINT32_MAX) return;
    if (!client->friend_online[friend_index]) return;
    if (client->friend_num[friend_index] == UINT32_MAX) return;
    if (client->invited[friend_index]) return;
    if (!client_is_group_connected(client)) return;

    Tox_Err_Group_Invite_Friend invite_err;
    bool ok = tox_group_invite_friend(client->tox, client->group_number, client->friend_num[friend_index], &invite_err);

    if (ok && invite_err == TOX_ERR_GROUP_INVITE_FRIEND_OK) {
        client->invited[friend_index] = true;
        log_client(client, "Invited %s to NGC group %u", clients[friend_index].name, client->group_number);
    }
}

/******************************************************************************
Tox callbacks (Using the simplified middleware API)
******************************************************************************/

static void self_connection_status_cb(Tox *tox, Tox_Connection connection_status, void *user_data)
{
    Client *client = user_data;

    bool connected = (connection_status != TOX_CONNECTION_NONE);

    if (client->network_connected != connected) {
        log_client(client, "Tox network connection status: %s",
                   connected ? (connection_status == TOX_CONNECTION_TCP ? "TCP" : "UDP") : "NONE");
    }

    client->network_connected = connected;
}

static void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length, void *user_data)
{
    Client *client = user_data;

    int from_index = find_client_index_by_public_key(public_key);
    const char *from_name = (from_index >= 0) ? (from_index == 3 ? "tox4" : (from_index == 4 ? "tox5" : clients[from_index].name)) : "unknown";

    Tox_Err_Friend_Add err;
    uint32_t friend_number = tox_friend_add_norequest(tox, public_key, &err);

    if (err == TOX_ERR_FRIEND_ADD_OK && friend_number != UINT32_MAX) {
        if (from_index >= 0 && from_index < NUM_CLIENTS) {
            client->friend_num[from_index] = friend_number;
        }

        log_client(client, "Accepted friend request from %s as friend number %u", from_name, friend_number);
    }
}

static void friend_connection_status_cb(Tox *tox, uint32_t friend_number, Tox_Connection connection_status, void *user_data)
{
    Client *client = user_data;

    int friend_index = find_friend_index(client, friend_number);
    if (friend_index < 0) return;

    bool online = (connection_status != TOX_CONNECTION_NONE);

    if (client->friend_online[friend_index] != online) {
        log_client(client, "Friend %s is now %s", clients[friend_index].name, online ? "ONLINE" : "OFFLINE");
    }

    client->friend_online[friend_index] = online;

    if (client->index == 0) invite_if_ready(client, friend_index);
}

static void group_invite_cb(Tox *tox, uint32_t friend_number, const uint8_t *invite_data, size_t invite_data_length, const uint8_t *group_name, size_t group_name_length, void *user_data)
{
    Client *client = user_data;

    int inviter_index = find_friend_index(client, friend_number);
    const char *inviter_name = (inviter_index >= 0) ? clients[inviter_index].name : "unknown";

    Tox_Err_Group_Invite_Accept err;
    uint32_t group_number = tox_group_invite_accept(tox, friend_number, invite_data, invite_data_length, (const uint8_t *)client->name, strlen(client->name), NULL, 0, &err);

    if (err == TOX_ERR_GROUP_INVITE_ACCEPT_OK && group_number != UINT32_MAX) {
        client->group_number = group_number;
        client->group_connected = false;

        log_client(client, "Accepted group invite from %s, local group number: %u", inviter_name, group_number);
    }
}

static void group_moderation_cb(Tox *tox,
                                uint32_t group_number,
                                uint32_t source_peer_id,
                                uint32_t target_peer_id,
                                Tox_Group_Mod_Event mod_type,
                                void *user_data)
{
    Client *client = user_data;

    if (client == NULL) {
        return;
    }

    if (client->group_number != group_number) {
        return;
    }

    log_client(client,
               "Group moderation event: type=%d source_peer=%u target_peer=%u",
               (int)mod_type,
               source_peer_id,
               target_peer_id);

    /* --- MIDDLEWARE INTEGRATION --- */
    bool we_were_kicked = mid_on_group_moderation(client->mid,
                                                  tox,
                                                  group_number,
                                                  source_peer_id,
                                                  target_peer_id,
                                                  mod_type);
    /* ------------------------------ */

    if (we_were_kicked) {
        log_client(client,
                   "I was kicked from group %u; clearing local group reference",
                   group_number);
        client->group_number = UINT32_MAX;
        client->group_connected = false;
    }
}

static void group_self_join_cb(Tox *tox, uint32_t group_number, void *user_data)
{
    Client *client = user_data;

    if (client->group_number == UINT32_MAX) client->group_number = group_number;

    client->group_connected = true;

    log_client(client, "Self joined NGC group %u (Callback fired!)", group_number);

    /* --- MIDDLEWARE INTEGRATION --- */
    mid_on_group_self_join(client->mid, tox, group_number, (const uint8_t *)client->name, strlen(client->name));
    /* ------------------------------ */

    if (client->index == 0) {
        for (int i = 1; i < NUM_CLIENTS; i++) invite_if_ready(client, i);
    }
}

static void group_peer_join_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, void *user_data)
{
    Client *client = user_data;

    log_client(client, "Peer %u joined NGC group %u", peer_id, group_number);

    /* --- MIDDLEWARE INTEGRATION --- */
    mid_on_group_peer_join(client->mid, tox, group_number, peer_id);
    /* ------------------------------ */
}

static void group_peer_exit_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, Tox_Group_Exit_Type exit_type, const uint8_t *name, size_t name_length, const uint8_t *part_message, size_t part_message_length, void *user_data)
{
    Client *client = user_data;

    log_client(client, "Peer %u exited NGC group %u (reason: %d)", peer_id, group_number, (int)exit_type);

    /* --- MIDDLEWARE INTEGRATION --- */
    mid_on_group_peer_exit(client->mid, tox, group_number, exit_type);
    /* ------------------------------ */

    /* --- PHASE 2 INTEGRATION --- */
    if (test_phase >= 2 && !tox3_exited_group) {
        tox3_exited_group = true;
        log_raw("Phase 2: Detected peer exit, assuming tox3 is offline.");
    }
    /* --------------------------- */
}

static void group_custom_packet_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *data, size_t length, void *user_data)
{
    Client *client = user_data;

    log_client(client, "Received group custom packet: %zu bytes from peer %u", length, peer_id);

    /* --- MIDDLEWARE INTEGRATION --- */
    mid_on_group_custom_packet(client->mid, tox, group_number, peer_id, data, length);
    /* ------------------------------ */
}

static void group_peer_name_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *name, size_t length, void *user_data)
{
    Client *client = user_data;

    log_client(client, "Peer %u changed name to: '%.*s'", peer_id, (int)length, (const char *)name);

    /* --- MIDDLEWARE INTEGRATION --- */
    mid_on_group_peer_name(client->mid, tox, group_number, peer_id);
    /* ------------------------------ */
}

static void group_connection_status_cb(Tox *tox, uint32_t group_number, int32_t status, void *user_data)
{
    Client *client = user_data;

    if (client->group_number == UINT32_MAX) {
        client->group_number = group_number;
    }

    if (status == 3) {
        client->group_connected = true;
        log_client(client, "Group %u is now connected (raw status=%d)", group_number, (int)status);

        if (client->index == 0) {
            for (int i = 1; i < NUM_CLIENTS; i++) {
                invite_if_ready(client, i);
            }
        }
    } else if (status == 2) {
        client->group_connected = false;
        log_client(client, "Group %u is connecting (raw status=%d)", group_number, (int)status);
    } else {
        client->group_connected = false;
        log_client(client, "Group %u is disconnected (raw status=%d)", group_number, (int)status);
    }
}

static void group_join_fail_cb(Tox *tox, uint32_t group_number, Tox_Group_Join_Fail fail_type, void *user_data)
{
    Client *client = user_data;

    log_client(client, "Join failed for group %u, reason: %d", group_number, (int)fail_type);
}

/******************************************************************************
Tox instance creation
******************************************************************************/

static void create_tox_instance(Client *client)
{
    Tox_Err_Options_New options_err;
    struct Tox_Options *options = tox_options_new(&options_err);

    if (options_err != TOX_ERR_OPTIONS_NEW_OK || options == NULL) exit(1);

    tox_options_set_ipv6_enabled(options, false);
    tox_options_set_udp_enabled(options, false);
    tox_options_set_local_discovery_enabled(options, false);
    tox_options_set_dht_announcements_enabled(options, false);
    tox_options_set_hole_punching_enabled(options, false);

    tox_options_set_start_port(options, 0);
    tox_options_set_end_port(options, 0);
    tox_options_set_tcp_port(options, 0);

    Tox_Err_New new_err;
    client->tox = tox_new(options, &new_err);

    tox_options_free(options);

    if (client->tox == NULL) exit(1);

    tox_self_get_address(client->tox, client->address);

    /* Initialize the opaque middleware state for this client */
    client->mid = mid_new("mid.save", NULL, 0);

    /* Register the peer-list-changed callback for this client */
    mid_set_peer_list_changed_cb(client->mid, peer_list_changed_cb, client);
}

static void register_callbacks(Tox *tox)
{
    tox_callback_self_connection_status(tox, self_connection_status_cb);
    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_connection_status(tox, friend_connection_status_cb);

    tox_callback_group_invite(tox, group_invite_cb);
    tox_callback_group_self_join(tox, group_self_join_cb);
    tox_callback_group_peer_join(tox, group_peer_join_cb);
    tox_callback_group_peer_exit(tox, group_peer_exit_cb);
    tox_callback_group_connection_status(tox, group_connection_status_cb);
    tox_callback_group_join_fail(tox, group_join_fail_cb);
    tox_callback_group_peer_name(tox, group_peer_name_cb);
    tox_callback_group_custom_packet(tox, group_custom_packet_cb);
    tox_callback_group_moderation(tox, group_moderation_cb);
}

/* Convert a hex string to raw bytes */
static int hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len)
{
    for (size_t i = 0; i < byte_len; i++) {
        unsigned int val;
        if (sscanf(hex + (i * 2), "%2x", &val) != 1) {
            return -1;
        }
        bytes[i] = (uint8_t)val;
    }
    return 0;
}

/******************************************************************************
Main
******************************************************************************/

static void signal_handler(int signum) { running = 0; }

int main(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    memset(clients, 0, sizeof(clients));
    memset(&tox4_client, 0, sizeof(tox4_client));
    memset(&tox5_client, 0, sizeof(tox5_client));

    clients[0].index = 0; clients[0].name = "tox1";
    clients[1].index = 1; clients[1].name = "tox2";
    clients[2].index = 2; clients[2].name = "tox3";

    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients[i].group_number = UINT32_MAX;
        clients[i].network_connected = false;
        clients[i].last_bootstrap = 0;

        for (int j = 0; j < NUM_CLIENTS; j++) {
            clients[i].friend_num[j] = UINT32_MAX;
            clients[i].friend_online[j] = false;
            clients[i].invited[j] = false;
        }
    }

    log_raw("Creating %d Tox instances in TCP-only mode", NUM_CLIENTS);

    for (int i = 0; i < NUM_CLIENTS; i++) {
        create_tox_instance(&clients[i]);
        register_callbacks(clients[i].tox);
    }

    log_raw("Bootstrapping all Tox instances to external bootstrap nodes");

    for (int i = 0; i < NUM_CLIENTS; i++) {
        bootstrap_client_to_network(&clients[i]);
    }

    log_raw("Waiting for all clients to connect to the Tox network...");

    time_t wait_start = time(NULL);
    bool all_net_connected = false;

    while (!all_net_connected && (time(NULL) - wait_start < 120)) {
        all_net_connected = true;

        for (int i = 0; i < NUM_CLIENTS; i++) {
            tox_iterate(clients[i].tox, &clients[i]);

            if (!clients[i].network_connected) {
                all_net_connected = false;
            }
        }

        for (int i = 0; i < NUM_CLIENTS; i++) {
            if (!clients[i].network_connected) {
                time_t now = time(NULL);
                if (clients[i].last_bootstrap == 0 || now - clients[i].last_bootstrap >= 10) {
                    bootstrap_client_to_network(&clients[i]);
                }
            }
        }

        if (!all_net_connected) {
            usleep(50000);
        }
    }

    if (!all_net_connected) {
        log_raw("Failed to connect all clients to the network within timeout. Exiting.");
        goto cleanup;
    }

    log_raw("All clients connected to the network. Proceeding with friend requests.");

    Tox_Err_Friend_Add friend_err;

    uint32_t friend2 = tox_friend_add(clients[0].tox, clients[1].address, (const uint8_t *)"hello", 5, &friend_err);
    if (friend_err == TOX_ERR_FRIEND_ADD_OK) {
        clients[0].friend_num[1] = friend2;
        log_client(&clients[0], "Sent friend request to tox2 (friend num %u)", friend2);
    }

    uint32_t friend3 = tox_friend_add(clients[0].tox, clients[2].address, (const uint8_t *)"hello", 5, &friend_err);
    if (friend_err == TOX_ERR_FRIEND_ADD_OK) {
        clients[0].friend_num[2] = friend3;
        log_client(&clients[0], "Sent friend request to tox3 (friend num %u)", friend3);
    }

#if 0
#define GROUP_CHAT_ID "154b3973bd0e66304fd6179a8a54759073649e09e6e368f0334fc6ed666ab762"
    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    if (hex_to_bytes(GROUP_CHAT_ID, chat_id, TOX_GROUP_CHAT_ID_SIZE) != 0) {
        fprintf(stderr, "[ERROR] Invalid group chat ID hex\n");
        return -1;
    }

    const char *nick = "tox1 ______ u";

    Tox_Err_Group_Join err;
    uint32_t group_number = tox_group_join(clients[0].tox, chat_id,
                                           (const uint8_t *)nick, strlen(nick),
                                           NULL, 0,   /* no password */
                                           &err);

    if (err == TOX_ERR_GROUP_JOIN_OK) {
#else
    const char *group_name = "TCP-NGC-Test";

    Tox_Err_Group_New group_err;
    uint32_t group_number = tox_group_new(clients[0].tox, TOX_GROUP_PRIVACY_STATE_PRIVATE, (const uint8_t *)group_name, strlen(group_name), (const uint8_t *)clients[0].name, strlen(clients[0].name), &group_err);

    if (group_err == TOX_ERR_GROUP_NEW_OK) {
#endif
        clients[0].group_number = group_number;
        clients[0].group_connected = true;

        /*
         * For the group founder, group_self_join_cb may not fire.
         * So we manually trigger the middleware self-join hook.
         */
        mid_on_group_self_join(clients[0].mid, clients[0].tox, group_number, (const uint8_t *)clients[0].name, strlen(clients[0].name));
    }

    time_t start_time = time(NULL);
    time_t all_connected_since = 0;
    time_t last_peer_table_print = 0;
    time_t phase_start_time = 0;

    while (running) {
        uint32_t min_interval = 50;

        for (int i = 0; i < NUM_CLIENTS; i++) {
            uint32_t interval = tox_iteration_interval(clients[i].tox);
            if (interval < min_interval) min_interval = interval;
        }

        if (min_interval == 0) min_interval = 1;
        if (min_interval > 100) min_interval = 100;

        usleep((useconds_t)min_interval * 1000);

        for (int i = 0; i < NUM_CLIENTS; i++) {
            /* --- PHASE 2+ INTEGRATION --- */
            if (test_phase >= 2 && test_phase < 6 && i == 2) {
                continue; /* Stop iterating tox3 to simulate disconnect */
            }
            /* ---------------------------- */

            tox_iterate(clients[i].tox, &clients[i]);

            /* --- MIDDLEWARE INTEGRATION --- */
            mid_iterate(clients[i].mid, clients[i].tox);
            /* ------------------------------ */
        }

        /* --- PHASE 3+ INTEGRATION --- */
        if (test_phase >= 3 && tox4_initialized) {
            tox_iterate(tox4_client.tox, &tox4_client);
            mid_iterate(tox4_client.mid, tox4_client.tox);

            if (!tox4_client.network_connected) {
                time_t now = time(NULL);
                if (tox4_client.last_bootstrap == 0 || now - tox4_client.last_bootstrap >= 10) {
                    bootstrap_client_to_network(&tox4_client);
                }
            }
        }
        /* ---------------------------- */

        /* --- PHASE 9+ INTEGRATION --- */
        if (test_phase >= 9 && tox5_initialized) {
            tox_iterate(tox5_client.tox, &tox5_client);
            mid_iterate(tox5_client.mid, tox5_client.tox);

            if (!tox5_client.network_connected) {
                time_t now = time(NULL);
                if (tox5_client.last_bootstrap == 0 || now - tox5_client.last_bootstrap >= 10) {
                    bootstrap_client_to_network(&tox5_client);
                }
            }
        }
        /* ---------------------------- */

        time_t table_now = time(NULL);

        if (table_now - last_peer_table_print >= 10) {
            for (int i = 0; i < NUM_CLIENTS; i++) {
                print_client_peer_table(&clients[i], clients[i].name);
            }
            print_client_peer_table(&tox4_client, tox4_client.name);
            print_client_peer_table(&tox5_client, tox5_client.name);
            last_peer_table_print = table_now;
        }

        for (int i = 0; i < NUM_CLIENTS; i++) {
            if (test_phase >= 2 && i == 2) continue;

            if (!clients[i].network_connected) {
                time_t now = time(NULL);
                if (clients[i].last_bootstrap == 0 || now - clients[i].last_bootstrap >= 30) {
                    bootstrap_client_to_network(&clients[i]);
                }
            }
        }

        /* --- PHASE STATE MACHINE --- */

        if (test_phase == 1) {
            bool synced = true;

            for (int i = 0; i < NUM_CLIENTS; i++) {
                uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
                if (!get_client_chat_id(&clients[i], cid) ||
                    mid_signed_count(clients[i].mid, cid) < NUM_CLIENTS) {
                    synced = false;
                    break;
                }
            }

            if (synced) {
                if (all_connected_since == 0) {
                    all_connected_since = time(NULL);
                    log_raw("Phase 1: All clients are connected to the NGC group and middleware is fully synced");
                } else if (time(NULL) - all_connected_since >= 10) {
                    log_raw("Phase 1: All clients stayed connected and synced for 10 seconds, success!");

                    test_phase = 2;
                    phase_start_time = time(NULL);

                    log_raw("Phase 2: Stopping tox3 iteration to simulate disconnect.");

                    all_connected_since = 0;
                }
            } else {
                all_connected_since = 0;
            }
        }

        if (test_phase == 2) {
            if (tox3_exited_group) {
                log_raw("Phase 2: tox3 is offline. Spawning tox4...");

                test_phase = 3;
                phase_start_time = time(NULL);
            } else if (time(NULL) - phase_start_time >= 120) {
                log_raw("Phase 2: Timeout waiting for tox3 to exit. Forcing Phase 3.");

                test_phase = 3;
                phase_start_time = time(NULL);
            }
        }

        if (test_phase == 3) {
            if (!tox4_initialized) {
                tox4_initialized = true;

                tox4_client.index = 3;
                tox4_client.name = "tox4";
                tox4_client.group_number = UINT32_MAX;
                tox4_client.network_connected = false;
                tox4_client.last_bootstrap = 0;
                tox4_client.group_connected = false;

                for (int j = 0; j < NUM_CLIENTS; j++) {
                    tox4_client.friend_num[j] = UINT32_MAX;
                    tox4_client.friend_online[j] = false;
                    tox4_client.invited[j] = false;
                }

                create_tox_instance(&tox4_client);
                register_callbacks(tox4_client.tox);
                bootstrap_client_to_network(&tox4_client);

                log_raw("Phase 3: tox4 created and bootstrapping.");
            }

            if (tox4_client.network_connected) {
                log_raw("Phase 3: tox4 connected to network. Proceeding to friend request.");

                test_phase = 4;
                phase_start_time = time(NULL);
            }
        }

        if (test_phase == 4) {
            static bool friend_request_sent = false;
            static uint32_t tox4_friend_num = UINT32_MAX;
            static bool tox4_online = false;
            static bool invited_tox4 = false;

            if (!friend_request_sent) {
                friend_request_sent = true;

                Tox_Err_Friend_Add err;
                tox4_friend_num = tox_friend_add(clients[0].tox, tox4_client.address, (const uint8_t *)"hello", 5, &err);

                if (err == TOX_ERR_FRIEND_ADD_OK) {
                    log_raw("Phase 4: tox1 sent friend request to tox4 (friend num %u)", tox4_friend_num);
                } else {
                    log_raw("Phase 4: Failed to add tox4 as friend, error: %d", err);
                }
            }

            Tox_Err_Friend_Query err;
            Tox_Connection status = tox_friend_get_connection_status(clients[0].tox, tox4_friend_num, &err);

            if (status != TOX_CONNECTION_NONE && !tox4_online) {
                tox4_online = true;
                log_raw("Phase 4: tox4 is now ONLINE as a friend.");
            }

            if (tox4_online && !invited_tox4) {
                if (client_is_group_connected(&clients[0])) {
                    invited_tox4 = true;

                    Tox_Err_Group_Invite_Friend err;
                    bool ok = tox_group_invite_friend(clients[0].tox, clients[0].group_number, tox4_friend_num, &err);

                    if (ok) {
                        log_raw("Phase 4: tox1 invited tox4 to the NGC group.");

                        test_phase = 5;
                        phase_start_time = time(NULL);
                        all_connected_since = 0;
                    } else {
                        log_raw("Phase 4: Failed to invite tox4, error: %d", err);
                    }
                }
            }
        }

        if (test_phase == 5) {
            if (tox4_client.group_number != UINT32_MAX && tox4_client.mid != NULL) {
                uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
                if (get_client_chat_id(&tox4_client, cid)) {
                    size_t count = mid_peer_count(tox4_client.mid, cid);
                    size_t signed_count = mid_signed_count(tox4_client.mid, cid);

                    if (count >= 3 && signed_count >= 3) {
                        if (all_connected_since == 0) {
                            all_connected_since = time(NULL);
                            log_raw("Phase 5: tox4 joined and middleware is syncing... (peers=%zu, signed=%zu)", count, signed_count);
                        } else if (time(NULL) - all_connected_since >= 15) {
                            log_raw("Phase 5: tox4 successfully received the persistent roster including offline tox3! Success!");

                            print_client_peer_table(&tox4_client, "tox4 FINAL");

                            test_phase = 6;
                            phase_start_time = time(NULL);
                            all_connected_since = 0;

                            log_raw("Phase 6: Resuming tox3 iteration to simulate reconnect...");
                        }
                    } else {
                        all_connected_since = 0;
                    }
                }
            }

            if (test_phase == 5 && time(NULL) - phase_start_time >= 120) {
                log_raw("Phase 5: Timeout waiting for tox4 to sync.");

                print_client_peer_table(&tox4_client, "tox4 TIMEOUT");

                test_phase = 6;
            }
        }

        if (test_phase == 6) {
            bool all_synced_phase6 = true;

            for (int i = 0; i < NUM_CLIENTS; i++) {
                if (clients[i].mid == NULL || clients[i].group_number == UINT32_MAX) {
                    all_synced_phase6 = false;
                    break;
                }

                uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
                if (!get_client_chat_id(&clients[i], cid)) {
                    all_synced_phase6 = false;
                    break;
                }

                if (mid_peer_count(clients[i].mid, cid) < 4) {
                    all_synced_phase6 = false;
                    break;
                }

                if (mid_signed_count(clients[i].mid, cid) < 4) {
                    all_synced_phase6 = false;
                    break;
                }

                if (mid_online_count(clients[i].mid, cid) < 4) {
                    all_synced_phase6 = false;
                    break;
                }
            }

            if (all_synced_phase6) {
                if (all_connected_since == 0) {
                    all_connected_since = time(NULL);
                    log_raw("Phase 6: tox3 is back online. Waiting for all clients to fully sync...");
                } else if (time(NULL) - all_connected_since >= 10) {
                    log_raw("Phase 6: All clients fully synced with tox3 back online! Success!");

                    for (int i = 0; i < NUM_CLIENTS; i++) {
                        print_client_peer_table(&clients[i], clients[i].name);
                    }

                    print_client_peer_table(&tox4_client, tox4_client.name);

                    test_phase = 7;
                    phase7_step = 0;
                    phase7_step_time = time(NULL);
                    phase7_sync_since = 0;

                    log_raw("Phase 7: tox2 will now gracefully leave the group.");
                }
            } else {
                all_connected_since = 0;
            }
        }

        /* --- PHASE 7: tox2 gracefully leaves --- */
        if (test_phase == 7) {
            /*
             * Step 0:
             * Capture tox2's group identity key before leaving.
             */
            if (phase7_step == 0) {
                if (clients[1].group_number != UINT32_MAX) {
                    Tox_Err_Group_Self_Query qerr;

                    if (tox_group_self_get_public_key(clients[1].tox,
                                                      clients[1].group_number,
                                                      tox2_identity,
                                                      &qerr) &&
                        qerr == TOX_ERR_GROUP_SELF_QUERY_OK) {
                        tox2_identity_known = true;
                        tox2_old_group_number = clients[1].group_number;

                        phase7_step = 1;
                        phase7_step_time = time(NULL);

                        log_raw("Phase 7: captured tox2 group identity, will send LEFT tombstone");
                    }
                } else {
                    log_raw("Phase 7: tox2 has no group number, cannot start graceful leave");
                    test_phase = 8;
                }
            }
            /*
             * Step 1:
             * tox2 broadcasts its signed LEFT tombstone.
             */
            else if (phase7_step == 1) {
                bool ok = mid_announce_leave(clients[1].mid,
                                             clients[1].tox,
                                             clients[1].group_number);

                if (ok) {
                    log_raw("Phase 7: tox2 sent signed LEFT tombstone");
                } else {
                    log_raw("Phase 7: WARNING: tox2 failed to send signed LEFT tombstone");
                }

                tox2_tombstone_sent = true;

                phase7_step = 2;
                phase7_step_time = time(NULL);
            }
            /*
             * Step 2:
             * Wait a few seconds while all clients keep iterating, so the
             * tombstone has time to be transmitted and processed.
             */
            else if (phase7_step == 2) {
                if (time(NULL) - phase7_step_time >= PHASE7_TOMBSTONE_FLUSH_SEC) {
                    log_raw("Phase 7: tombstone flush wait done, calling tox_group_leave() for tox2");

                    phase7_step = 3;
                    phase7_step_time = time(NULL);
                }
            }
            /*
             * Step 3:
             * tox2 calls tox_group_leave() via the unified helper.
             * This ensures mid_on_group_delete() is always called.
             */
            else if (phase7_step == 3) {
                if (time(NULL) - phase7_step_time >= 1) {
                    client_leave_group(&clients[1]);

                    log_raw("Phase 7: tox2 left the Tox group");

                    tox2_group_left = true;

                    phase7_step = 4;
                    phase7_step_time = time(NULL);
                    phase7_sync_since = 0;
                }
            }
            /*
             * Step 4:
             * Wait until tox1, tox3, and tox4 all have the signed LEFT tombstone.
             */
            else if (phase7_step == 4) {
                bool t1_has_tombstone = has_tombstone(&clients[0], tox2_identity);
                bool t3_has_tombstone = has_tombstone(&clients[2], tox2_identity);
                bool t4_has_tombstone = has_tombstone(&tox4_client, tox2_identity);

                if (t1_has_tombstone && t3_has_tombstone && t4_has_tombstone) {
                    if (phase7_sync_since == 0) {
                        phase7_sync_since = time(NULL);
                        log_raw("Phase 7: tox2 LEFT tombstone seen on tox1, tox3, and tox4; waiting to confirm stable");
                    } else if (time(NULL) - phase7_sync_since >= PHASE7_SYNC_WAIT_SEC) {
                        log_raw("Phase 7: tox2 LEFT tombstone fully synced! Success!");

                        print_client_peer_table(&clients[0], "tox1 after tox2 left");
                        print_client_peer_table(&clients[2], "tox3 after tox2 left");
                        print_client_peer_table(&tox4_client, "tox4 after tox2 left");

                        /*
                         * Phase 8:
                         * Kick tox4 from the group and verify that the middleware handles it.
                         */
                        test_phase = 8;
                        phase8_step = 0;
                        phase8_step_time = time(NULL);
                        phase8_sync_since = 0;
                        phase8_last_kick_attempt = 0;

                        tox4_identity_known = false;
                        tox4_kicked = false;
                        tox4_cleanup_done = false;

                        log_raw("Phase 8: will now kick tox4 from the group");
                    }
                } else {
                    phase7_sync_since = 0;

                    if (time(NULL) - phase7_step_time >= PHASE7_TIMEOUT_SEC) {
                        log_raw("Phase 7: timeout waiting for tox2 tombstone sync");

                        print_client_peer_table(&clients[0], "tox1 timeout");
                        print_client_peer_table(&clients[2], "tox3 timeout");
                        print_client_peer_table(&tox4_client, "tox4 timeout");

                        test_phase = 8;
                    }
                }
            }
        }

        /* --- PHASE 8: kick tox4 --- */
        if (test_phase == 8) {
            /*
             * Step 0:
             * Capture tox4's group identity key before kicking it.
             */
            if (phase8_step == 0) {
                if (tox4_initialized && tox4_client.group_number != UINT32_MAX) {
                    Tox_Err_Group_Self_Query qerr;

                    if (tox_group_self_get_public_key(tox4_client.tox,
                                                      tox4_client.group_number,
                                                      tox4_identity,
                                                      &qerr) &&
                        qerr == TOX_ERR_GROUP_SELF_QUERY_OK) {
                        tox4_identity_known = true;

                        phase8_step = 1;
                        phase8_step_time = time(NULL);
                        phase8_last_kick_attempt = 0;

                        log_raw("Phase 8: captured tox4 identity, preparing to kick");
                    } else if (time(NULL) - phase8_step_time >= 30) {
                        log_raw("Phase 8: timeout capturing tox4 identity");
                        test_phase = 9;
                    }
                } else {
                    if (time(NULL) - phase8_step_time >= 30) {
                        log_raw("Phase 8: tox4 is not in a group, skipping kick phase");
                        test_phase = 9;
                    }
                }
            }
            /*
             * Step 1:
             * tox1 kicks tox4.
             */
            else if (phase8_step == 1) {
                if (time(NULL) - phase8_last_kick_attempt >= 5) {
                    phase8_last_kick_attempt = time(NULL);

                    Tox_Err_Group_Peer_Query perr;
                    uint32_t tox4_peer_id =
                        tox_group_peer_by_public_key(clients[0].tox,
                                                     clients[0].group_number,
                                                     tox4_identity,
                                                     &perr);

                    if (perr == TOX_ERR_GROUP_PEER_QUERY_OK) {
                        Tox_Err_Group_Mod_Kick_Peer kerr;

                        bool ok = tox_group_mod_kick_peer(clients[0].tox,
                                                          clients[0].group_number,
                                                          tox4_peer_id,
                                                          &kerr);

                        if (ok && kerr == TOX_ERR_GROUP_MOD_KICK_PEER_OK) {
                            tox4_kicked = true;

                            log_raw("Phase 8: tox1 kicked tox4");

                            /*
                             * IMPORTANT:
                             *
                             * The peer that INITIATES the kick does NOT receive the
                             * group_moderation event (see tox.h). Therefore the
                             * middleware cannot auto-delete the kicked peer on the
                             * kicker's side via mid_on_group_moderation().
                             *
                             * As the kicker, tox1 must explicitly delete tox4 from
                             * its own middleware roster.
                             */
                            uint8_t cid0[TOX_GROUP_CHAT_ID_SIZE];
                            if (get_client_chat_id(&clients[0], cid0)) {
                                mid_delete_peer_by_identity(clients[0].mid,
                                                            cid0,
                                                            tox4_identity);

                                log_raw("Phase 8: tox1 deleted tox4 from its own middleware roster");
                            }

                            phase8_step = 2;
                            phase8_step_time = time(NULL);
                            phase8_sync_since = 0;
                        } else {
                            log_raw("Phase 8: tox_group_mod_kick_peer failed, retrying");
                        }
                    } else {
                        log_raw("Phase 8: tox4 peer id not found on tox1, retrying");
                    }
                }

                if (time(NULL) - phase8_step_time >= 120) {
                    log_raw("Phase 8: timeout while trying to kick tox4");
                    test_phase = 9;
                }
            }
            /*
             * Step 2:
             * Wait until tox4 is removed from the middleware roster of tox1 and tox3.
             *
             * This expects your middleware to treat TOX_GROUP_EXIT_TYPE_KICK as a
             * permanent removal from the roster.
             */
            else if (phase8_step == 2) {
                bool absent_on_tox1 = peer_absent(&clients[0], tox4_identity);
                bool absent_on_tox3 = peer_absent(&clients[2], tox4_identity);

                if (absent_on_tox1 && absent_on_tox3) {
                    if (phase8_sync_since == 0) {
                        phase8_sync_since = time(NULL);
                    } else if (time(NULL) - phase8_sync_since >= 10) {
                        log_raw("Phase 8: tox4 removed from remaining peers' middleware");

                        phase8_step = 3;
                        phase8_step_time = time(NULL);
                        phase8_sync_since = 0;
                    }
                } else {
                    phase8_sync_since = 0;

                    if (time(NULL) - phase8_step_time >= 120) {
                        log_raw("Phase 8: timeout waiting for tox4 removal from remaining peers");

                        print_client_peer_table(&clients[0], "tox1 timeout after kick");
                        print_client_peer_table(&clients[2], "tox3 timeout after kick");

                        test_phase = 9;
                    }
                }
            }
            /*
             * Step 3:
             * Wait until tox4 itself is no longer connected to the group.
             */
            else if (phase8_step == 3) {
                bool tox4_gone = false;

                if (tox4_client.group_number == UINT32_MAX) {
                    tox4_gone = true;
                } else {
                    Tox_Err_Group_Is_Connected ierr;
                    int32_t st = tox_group_is_connected(tox4_client.tox,
                                                        tox4_client.group_number,
                                                        &ierr);

                    tox4_gone = (ierr != TOX_ERR_GROUP_IS_CONNECTED_OK || st != 1);
                }

                if (tox4_gone) {
                    if (phase8_sync_since == 0) {
                        phase8_sync_since = time(NULL);
                    } else if (time(NULL) - phase8_sync_since >= 10) {
                        log_raw("Phase 8: tox4 is no longer connected to the group");

                        phase8_step = 4;
                    }
                } else {
                    phase8_sync_since = 0;

                    if (time(NULL) - phase8_step_time >= 120) {
                        log_raw("Phase 8: timeout waiting for tox4 to disconnect");
                        test_phase = 9;
                    }
                }
            }
            /*
             * Step 4:
             * Clean up tox4 middleware state and print final tables.
             */
            else if (phase8_step == 4) {
                if (!tox4_cleanup_done) {
                    if (tox4_client.group_number != UINT32_MAX) {
                        print_client_peer_table(&tox4_client, "tox4 after kick before cleanup");

                        /*
                         * tox4 is no longer part of the group.
                         *
                         * Toxcore may already have removed the group internally.
                         * We clean the middleware state for that group via the
                         * unified leave helper.
                         */
                        client_leave_group(&tox4_client);

                        tox4_cleanup_done = true;

                        log_raw("Phase 8: tox4 middleware group deleted after kick");
                    }
                }

                print_client_peer_table(&clients[0], "tox1 after kicking tox4");
                print_client_peer_table(&clients[2], "tox3 after kicking tox4");

                log_raw("Phase 8: kick test complete");

                test_phase = 9;
            }
        }
        /* ---------------------------- */

        /* --- PHASE 9: tox5 with broken UTF-8 name --- */
        if (test_phase == 9) {
            if (!tox5_initialized) {
                tox5_initialized = true;

                tox5_client.index = 4;
                tox5_client.name = tox5_broken_name;
                tox5_client.group_number = UINT32_MAX;
                tox5_client.network_connected = false;
                tox5_client.last_bootstrap = 0;
                tox5_client.group_connected = false;

                for (int j = 0; j < NUM_CLIENTS; j++) {
                    tox5_client.friend_num[j] = UINT32_MAX;
                    tox5_client.friend_online[j] = false;
                    tox5_client.invited[j] = false;
                }

                create_tox_instance(&tox5_client);
                register_callbacks(tox5_client.tox);
                bootstrap_client_to_network(&tox5_client);

                log_raw("Phase 9: tox5 created with broken UTF-8 name and bootstrapping.");
            }

            if (tox5_initialized && tox5_client.network_connected) {
                static bool tox5_friend_req_sent = false;
                static uint32_t tox5_friend_num = UINT32_MAX;
                static bool tox5_online = false;
                static bool invited_tox5 = false;
                static time_t phase9_sync_since = 0;

                if (!tox5_friend_req_sent) {
                    tox5_friend_req_sent = true;

                    Tox_Err_Friend_Add err;
                    tox5_friend_num = tox_friend_add(clients[0].tox, tox5_client.address, (const uint8_t *)"hello", 5, &err);

                    if (err == TOX_ERR_FRIEND_ADD_OK) {
                        log_raw("Phase 9: tox1 sent friend request to tox5 (friend num %u)", tox5_friend_num);
                    } else {
                        log_raw("Phase 9: Failed to add tox5 as friend, error: %d", err);
                    }
                }

                Tox_Err_Friend_Query err;
                Tox_Connection status = tox_friend_get_connection_status(clients[0].tox, tox5_friend_num, &err);

                if (status != TOX_CONNECTION_NONE && !tox5_online) {
                    tox5_online = true;
                    log_raw("Phase 9: tox5 is now ONLINE as a friend.");
                }

                if (tox5_online && !invited_tox5) {
                    if (client_is_group_connected(&clients[0])) {
                        invited_tox5 = true;

                        Tox_Err_Group_Invite_Friend err;
                        bool ok = tox_group_invite_friend(clients[0].tox, clients[0].group_number, tox5_friend_num, &err);

                        if (ok) {
                            log_raw("Phase 9: tox1 invited tox5 to the NGC group.");
                        } else {
                            log_raw("Phase 9: Failed to invite tox5, error: %d", err);
                        }
                    }
                }

                if (tox5_client.group_number != UINT32_MAX && tox5_client.mid != NULL) {
                    uint8_t cid[TOX_GROUP_CHAT_ID_SIZE];
                    if (get_client_chat_id(&tox5_client, cid)) {
                        size_t count = mid_peer_count(tox5_client.mid, cid);

                        if (count >= 2) {
                            if (phase9_sync_since == 0) {
                                phase9_sync_since = time(NULL);
                                log_raw("Phase 9: tox5 joined and middleware is syncing... (peers=%zu)", count);
                            } else if (time(NULL) - phase9_sync_since >= 15) {
                                log_raw("Phase 9: tox5 synced successfully with broken UTF-8 name! Success!");

                                print_client_peer_table(&tox5_client, "tox5 FINAL");
                                print_client_peer_table(&clients[0], "tox1 after tox5 joined");

                                test_phase = 10;
                            }
                        } else {
                            phase9_sync_since = 0;
                        }
                    }
                }
            }
        }
        /* ---------------------------- */

        if (test_phase == 10) {
            log_raw("Test completed successfully.");
            break;
        }

        if (time(NULL) - start_time >= 900) {
            log_raw("Global timeout reached (900 seconds), exiting");
            break;
        }
    }

cleanup:
    log_raw("Cleaning up");

    /*
     * Leave any groups we are still in before freeing middleware.
     * This ensures mid_on_group_delete() is called for every group
     * so no middleware state leaks, even on early exit / signal.
     */
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (clients[i].group_number != UINT32_MAX) {
            client_leave_group(&clients[i]);
        }
    }

    if (tox4_initialized && tox4_client.group_number != UINT32_MAX) {
        client_leave_group(&tox4_client);
    }

    if (tox5_initialized && tox5_client.group_number != UINT32_MAX) {
        client_leave_group(&tox5_client);
    }

    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (clients[i].mid != NULL) {
           if (!mid_save(clients[i].mid, NULL, 0)) {
                fprintf(stderr, "Warning: Failed to save middleware state to disk!\n");
            }
            mid_free(clients[i].mid);
            clients[i].mid = NULL;
        }

        if (clients[i].tox != NULL) {
            tox_kill(clients[i].tox);
            clients[i].tox = NULL;
        }
    }

    if (tox4_initialized) {
        if (tox4_client.mid != NULL) {
            mid_free(tox4_client.mid);
            tox4_client.mid = NULL;
        }

        if (tox4_client.tox != NULL) {
            tox_kill(tox4_client.tox);
            tox4_client.tox = NULL;
        }
    }

    if (tox5_initialized) {
        if (tox5_client.mid != NULL) {
            mid_free(tox5_client.mid);
            tox5_client.mid = NULL;
        }

        if (tox5_client.tox != NULL) {
            tox_kill(tox5_client.tox);
            tox5_client.tox = NULL;
        }
    }

    return 0;
}

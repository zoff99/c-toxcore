
#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include <tox/tox.h>

static int self_online = 0;
static int self_revd_custom_pkts = 0;

struct Node1 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes1[] = {
{ "2604:a880:1:20::32f:1001", "BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F", 33445, 33445 },
{ "tox.kurnevsky.net", "82EF82BA33445A1F91A7DB27189ECFC0C013E06E3DA71F588ED692BED625EC23", 33445, 33445 },
{ "tox1.mf-net.eu","B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506",33445,33445},
{ "tox3.plastiras.org","4B031C96673B6FF123269FF18F2847E1909A8A04642BBECD0189AC8AEEADAF64",33445,3389},
    { NULL, NULL, 0, 0 }
};

static void self_connection_change_callback(Tox *tox, TOX_CONNECTION status, void *userdata)
{
    switch (status) {
        case TOX_CONNECTION_NONE:
            printf("RCV:Lost connection to the Tox network.\n");
            self_online = 0;
            break;
        case TOX_CONNECTION_TCP:
            printf("RCV:Connected using TCP.\n");
            self_online = 1;
            break;
        case TOX_CONNECTION_UDP:
            printf("RCV:Connected using UDP.\n");
            self_online = 2;
            break;
    }
}

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
    printf("RCV:C-TOXCORE:%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}

static void group_invite_cb(Tox *tox, uint32_t friend_number, const uint8_t *invite_data, size_t length,
                                 const uint8_t *group_name, size_t group_name_length, void *userdata)
{
    Tox_Err_Group_Invite_Accept error;
    tox_group_invite_accept(tox, friend_number, invite_data, length,
                                 (const uint8_t *)"RCV", strlen("RCV"),
                                 NULL, 0,
                                 &error);
    printf("RCV:tox_group_invite_accept:%d\n", error);
}

static void group_self_join_cb(Tox *tox, uint32_t group_number, void *userdata)
{
    printf("RCV:You joined group %d\n", group_number);
}

static void group_peer_join_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, void *user_data)
{
    printf("RCV:Peer %d joined group %d\n", peer_id, group_number);
}

static void group_custom_packet_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *data,
                                        size_t length, void *user_data)
{
    printf("RCV:custom pkt received len=%d\n", (int)length);
    self_revd_custom_pkts++;
}

static void group_custom_private_packet_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, const uint8_t *data,
        size_t length, void *user_data)
{
    printf("RCV:custom private pkt received len=%d\n", (int)length);
    self_revd_custom_pkts++;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("RCV:--START--\n");
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
    printf("RCV:init Tox\n");
    Tox *tox = tox_new(&options, NULL);
    // ----- CALLBACKS -----
    tox_callback_self_connection_status(tox, self_connection_change_callback);
    tox_callback_group_invite(tox, group_invite_cb);
    tox_callback_group_peer_join(tox, group_peer_join_cb);
    tox_callback_group_self_join(tox, group_self_join_cb);
    tox_callback_group_custom_packet(tox, group_custom_packet_cb);
    tox_callback_group_custom_private_packet(tox, group_custom_private_packet_cb);
    // ----- CALLBACKS -----
    // ----- bootstrap -----
    printf("RCV:Tox bootstrapping\n");
    for (int i = 0; nodes1[i].ip; i++)
    {
        uint8_t *key = (uint8_t *)calloc(1, 100);
        hex_string_to_bin2(nodes1[i].key, key);
        if (!key)
        {
            continue;
        }
        tox_bootstrap(tox, nodes1[i].ip, nodes1[i].udp_port, key, NULL);
        if (nodes1[i].tcp_port != 0)
        {
            tox_add_tcp_relay(tox, nodes1[i].ip, nodes1[i].tcp_port, key, NULL);
        }
        free(key);
    }
    // ----- bootstrap -----
    tox_iterate(tox, NULL);
    // ----------- wait for Tox to come online -----------
    while (1 == 1)
    {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
        if (self_online > 0)
        {
            break;
        }
    }
    printf("RCV:Tox online\n");
    // ----------- wait for Tox to come online -----------


    // ----------- custom part -----------
    char *hexString = argv[1];
    int hexLength = strlen(hexString);
    int bufferSize = hexLength / 2;
    unsigned char *byteBuffer = (unsigned char *)calloc(1, bufferSize);
    for (int i = 0; i < bufferSize; i++) {
        sscanf(hexString + (2 * i), "%2hhx", &byteBuffer[i]);
    }
    TOX_ERR_FRIEND_ADD error;
    uint32_t friendnum = tox_friend_add(tox, (uint8_t *)byteBuffer,
                                        (uint8_t *)" ",
                                        (size_t)strlen(" "),
                                        &error);
    printf("RCV:friendnum=%d error=%d\n", friendnum, error);
    free(byteBuffer);
    // ----------- custom part -----------

    while (self_revd_custom_pkts < 2) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    printf("RCV:received %d custom packets already\n", self_revd_custom_pkts);

    long count = 0;
    long max_count = 2000;
    while (count < max_count) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
        count++;
    }

    tox_kill(tox);
    printf("RCV:killed Tox\n");
    printf("RCV:--END--\n");
} 


#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include <tox/tox.h>

static int self_online = 0;
static int friend_online = 0;
static int group_self_join = 0;
static int group_peer_join = 0;

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
            printf("SDR:Lost connection to the Tox network.\n");
            self_online = 0;
            break;
        case TOX_CONNECTION_TCP:
            printf("SDR:Connected using TCP.\n");
            self_online = 1;
            break;
        case TOX_CONNECTION_UDP:
            printf("SDR:Connected using UDP.\n");
            self_online = 2;
            break;
    }
}

static void friend_connection_status_callback(Tox *tox, uint32_t friend_number, Tox_Connection connection_status, void *user_data)
{
    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            printf("SDR:Lost connection to friend.\n");
            friend_online = 0;
            break;
        case TOX_CONNECTION_TCP:
            printf("SDR:Connected to friend using TCP.\n");
            friend_online = 1;
            break;
        case TOX_CONNECTION_UDP:
            printf("SDR:Connected to friend using UDP.\n");
            friend_online = 2;
            break;
    }
}

static void friend_request_callback(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length, void *user_data)
{
    tox_friend_add_norequest(tox, public_key, NULL);
    printf("SDR:friend added\n");
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

static void get_my_toxid(Tox *tox, char *toxid_str)
{
    uint8_t tox_id_bin[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox, tox_id_bin);
    char tox_id_hex_local[TOX_ADDRESS_SIZE * 2 + 1];
    sodium_bin2hex(tox_id_hex_local, sizeof(tox_id_hex_local), tox_id_bin, sizeof(tox_id_bin));

    for (size_t i = 0; i < sizeof(tox_id_hex_local) - 1; i ++)
    {
        tox_id_hex_local[i] = toupper(tox_id_hex_local[i]);
    }

    snprintf(toxid_str, (size_t)(TOX_ADDRESS_SIZE * 2 + 1), "%s", (const char *)tox_id_hex_local);
}

static void print_tox_id(Tox *tox)
{
    char tox_id_hex[TOX_ADDRESS_SIZE * 2 + 1];
    get_my_toxid(tox, tox_id_hex);

    printf("SDR:--MyToxID--:%s\n", tox_id_hex);

    // write ToxID to toxid text file -----------
    char *my_toxid_filename_txt = "z_sender_toxid.txt";
    FILE *fp = fopen(my_toxid_filename_txt, "wb");

    if (fp)
    {
        fprintf(fp, "%s", tox_id_hex);
        fclose(fp);
    }

    // write ToxID to toxid text file -----------
}

static void tox_log_cb__custom(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func,
                        const char *message, void *user_data)
{
    printf("SDR:C-TOXCORE:%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}

static void group_invite_cb(Tox *tox, uint32_t friend_number, const uint8_t *invite_data, size_t length,
                                 const uint8_t *group_name, size_t group_name_length, void *userdata)
{
    Tox_Err_Group_Invite_Accept error;
    tox_group_invite_accept(tox, friend_number, invite_data, length,
                                 (const uint8_t *)"SDR", strlen("SDR"),
                                 NULL, 0,
                                 &error);
    printf("SDR:tox_group_invite_accept:%d\n", error);
}

static void group_self_join_cb(Tox *tox, uint32_t group_number, void *userdata)
{
    printf("SDR:You joined group %d\n", group_number);
    group_self_join = 1;
}

static void group_peer_join_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, void *user_data)
{
    printf("SDR:Peer %d joined group %d\n", peer_id, group_number);
    group_peer_join = 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("SDR:--START--\n");
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
    printf("SDR:init Tox\n");
    Tox *tox = tox_new(&options, NULL);
    // ----- CALLBACKS -----
    tox_callback_self_connection_status(tox, self_connection_change_callback);
    tox_callback_friend_connection_status(tox, friend_connection_status_callback);
    tox_callback_friend_request(tox, friend_request_callback);
    tox_callback_group_invite(tox, group_invite_cb);
    tox_callback_group_peer_join(tox, group_peer_join_cb);
    tox_callback_group_self_join(tox, group_self_join_cb);
    // ----- CALLBACKS -----
    // ----- bootstrap -----
    printf("SDR:Tox bootstrapping\n");
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
    printf("SDR:Tox online\n");
    // ----------- wait for Tox to come online -----------

    print_tox_id(tox);

    while (friend_online < 1) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    printf("SDR:friend online\n");
    tox_group_new(tox, TOX_GROUP_PRIVACY_STATE_PRIVATE, "group1", strlen("group1"), "SDR", strlen("SDR"), NULL);

    //while (group_self_join == 0) {
    //    tox_iterate(tox, NULL);
    //    usleep(tox_iteration_interval(tox));
    //}

    for (int j=0;j<20;j++) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    printf("SDR:group online with myself in it\n");
    tox_group_invite_friend(tox, 0, 0, NULL);

    while (group_peer_join == 0) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    printf("SDR:peer joined group, sending custom packets ...\n");

    Tox_Err_Group_Send_Custom_Packet error1;
    Tox_Err_Group_Send_Custom_Private_Packet error2;
    const int data_grp_pkt_len = 60000;
    const int data_grp_pvt_pkt_len = 60000;
    uint8_t *data_grp_pkt = (uint8_t *)calloc(1, data_grp_pkt_len);
    uint8_t *data_grp_pvt_pkt = (uint8_t *)calloc(1, data_grp_pvt_pkt_len);

    tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, 499, &error1);
    tox_group_send_custom_private_packet(tox, 0, 1, true, data_grp_pvt_pkt, 499, &error2);
    //tox_group_send_custom_packet(tox, 0, false, data_grp_pkt, 499, &error1);
    //tox_group_send_custom_private_packet(tox, 0, 1, false, data_grp_pvt_pkt, 499, &error2);
    tox_iterate(tox, NULL);
    usleep(tox_iteration_interval(tox));

    tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, 1372, &error1);
    tox_group_send_custom_private_packet(tox, 0, 1, true, data_grp_pvt_pkt, 1372, &error2);

    for (int k=1373;k<1375;k++)
    {
        tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, k, &error1);
        tox_group_send_custom_private_packet(tox, 0, 1, true, data_grp_pvt_pkt, k, &error2);
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    for (int k=38000;k<40001;k=k+1000)
    {
        tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, k, &error1);
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    for (int k=39998;k<40021;k++)
    {
        tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, k, &error1);
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    tox_group_send_custom_packet(tox, 0, true, data_grp_pkt, data_grp_pkt_len, &error1);
    tox_iterate(tox, NULL);
    usleep(tox_iteration_interval(tox));

    free(data_grp_pkt);
    free(data_grp_pvt_pkt);

    while (1==1) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox));
    }

    tox_kill(tox);
    printf("SDR:killed Tox\n");
    printf("SDR:--END--\n");
} 

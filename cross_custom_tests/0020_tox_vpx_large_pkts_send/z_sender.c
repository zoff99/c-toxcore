
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include <tox/tox.h>
#include <tox/toxav.h>

static int self_online = 0;
static int friend_online = 0;
static int group_self_join = 0;
static int group_peer_join = 0;

static int global_audio_bit_rate = 32;
static int global_video_bit_rate = 180;

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

static void t_toxav_call_cb(ToxAV *av, uint32_t friend_number, bool audio_enabled, bool video_enabled, void *user_data)
{
}

static void t_toxav_call_state_cb(ToxAV *av, uint32_t friend_number, uint32_t state, void *user_data)
{
}

static void t_toxav_receive_video_frame_cb(ToxAV *av, uint32_t friend_number,
        uint16_t width, uint16_t height,
        uint8_t const *y, uint8_t const *u, uint8_t const *v,
        int32_t ystride, int32_t ustride, int32_t vstride,
        void *user_data)
{
}

static void t_toxav_receive_audio_frame_cb(ToxAV *av, uint32_t friend_number,
        int16_t const *pcm,
        size_t sample_count,
        uint8_t channels,
        uint32_t sampling_rate,
        void *user_data)
{
}

static void t_toxav_call_comm_cb(ToxAV *av, uint32_t friend_number, TOXAV_CALL_COMM_INFO comm_value,
                                 int64_t comm_number, void *user_data)
{
    if (comm_value == TOXAV_CALL_COMM_DECODER_IN_USE_VP8)
    {
        printf("SDR:decoder:--VP8--\n");
    }
    else if (comm_value == TOXAV_CALL_COMM_ENCODER_IN_USE_VP8)
    {
        printf("SDR:encoder:--VP8--\n");
    }
    else if (comm_value == TOXAV_CALL_COMM_DECODER_IN_USE_H264)
    {
        printf("SDR:decoder:H264\n");
    }
    else if (comm_value == TOXAV_CALL_COMM_ENCODER_IN_USE_H264)
    {
        printf("SDR:encoder:H264\n");
    }
}

uint32_t n_r(const uint32_t upper_bound)
{
    return rand() % upper_bound;
}

void rvbuf(uint8_t *buf, size_t size)
{
    for (int i=0; i < size; i++)
    {
        // random value 1..255
        *buf = (uint8_t)((n_r(255)) + 1);
        buf++;
    }
}

void rvbuf0(uint8_t *buf, size_t size)
{
    for (int i=0; i < size; i++)
    {
        // random value 0..255
        *buf = (uint8_t)(n_r(256));
        buf++;
    }
}

int main(int argc, char *argv[])
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
    // ----- CALLBACKS -----

    // ------ init ToxAV ---
    TOXAV_ERR_NEW rc;
    printf("SDR:new Tox AV\n");
    ToxAV *mytox_av = toxav_new(tox, &rc);
    if (rc != TOXAV_ERR_NEW_OK)
    {
        printf("SDR: Error at toxav_new: %d\n", rc);
    }
    toxav_audio_iterate_seperation(mytox_av, true);

    toxav_callback_call(mytox_av, t_toxav_call_cb, NULL);
    toxav_callback_call_state(mytox_av, t_toxav_call_state_cb, NULL);
    toxav_callback_video_receive_frame(mytox_av, t_toxav_receive_video_frame_cb, NULL);
    toxav_callback_audio_receive_frame(mytox_av, t_toxav_receive_audio_frame_cb, NULL);
    toxav_callback_call_comm(mytox_av, t_toxav_call_comm_cb, NULL);
    // ------ init ToxAV ---


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
        usleep(tox_iteration_interval(tox)*1000);
        if (self_online > 0)
        {
            break;
        }
    }
    printf("SDR:Tox online\n");
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
    printf("SDR:friendnum=%d error=%d\n", friendnum, error);
    free(byteBuffer);
    // ----------- custom part -----------

    while (friend_online < 1) {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox)*1000);
    }

    printf("SDR:friend online\n");

    toxav_call(mytox_av, 0, global_audio_bit_rate, global_video_bit_rate, &error);
    printf("SDR:toxav_call:res=%d\n", error);

    int w = 1920; // 3840;
    int h = 1080; // 2160;
    int bufsize = (w * h) * 3 / 2;
    uint8_t *v = calloc(1, bufsize);

    rvbuf0(v, bufsize);

    TOXAV_ERR_SEND_FRAME *error_v;

    for (int j = 0; j < 100; j++) {
        tox_iterate(tox, NULL);
        toxav_iterate(mytox_av);
        toxav_audio_iterate(mytox_av);

        rvbuf0(v, bufsize);
        toxav_video_send_frame(mytox_av, 0, w, h , v, v, v, &error_v);

        usleep(2*1000);
    }

    free(v);

    toxav_kill(mytox_av);
    tox_kill(tox);
    printf("SDR:killed Tox\n");
    printf("SDR:--END--\n");
} 

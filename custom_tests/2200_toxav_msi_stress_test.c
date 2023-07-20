

#define _GNU_SOURCE


#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <pthread.h>

#include <semaphore.h>
#include <signal.h>
#include <linux/sched.h>

#include <sodium.h>

#include <tox/tox.h>
#include <tox/toxav.h>

#define CURRENT_LOG_LEVEL 9 // 0 -> error, 1 -> warn, 2 -> info, 9 -> debug
const char *log_filename = "output.log";
const char *savedata_filename1 = "savedata1.tox";
const char *savedata_filename2 = "savedata2.tox";
FILE *logfile = NULL;

uint8_t s_num1 = 1;
uint8_t s_num2 = 2;
long a_frames_rvcd = 0;
long v_frames_rvcd = 0;
long a_frames_sent = 0;
long v_frames_sent = 0;

int s_online[3] = { 0, 0, 0};
int f_online[3] = { 0, 0, 0};
int f_join_other[3] = { 0, 0, 0};
int f_join_self[3] = { 0, 0, 0};

int toxav_video_thread_stop = 0;
int toxav_audioiterate_thread_stop = 0;

struct Node1 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes1[] = {
{ "tox.novg.net", "D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463", 33445, 33445 },
{ "bg.tox.dcntrlzd.network", "20AD2A54D70E827302CDF5F11D7C43FA0EC987042C36628E64B2B721A1426E36", 33445, 33445 },
{"91.219.59.156","8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832",33445,33445},
{"85.143.221.42","DA4E4ED4B697F2E9B000EEFE3A34B554ACD3F45F5C96EAEA2516DD7FF9AF7B43",33445,33445},
{"tox.initramfs.io","3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25",33445,33445},
{"144.217.167.73","7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C",33445,33445},
{"tox.abilinski.com","10C00EB250C3233E343E2AEBA07115A5C28920E9C8D29492F6D00B29049EDC7E",33445,33445},
{"tox.novg.net","D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463",33445,33445},
{"198.199.98.108","BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F",33445,33445},
{"tox.kurnevsky.net","82EF82BA33445A1F91A7DB27189ECFC0C013E06E3DA71F588ED692BED625EC23",33445,33445},
{"81.169.136.229","E0DB78116AC6500398DDBA2AEEF3220BB116384CAB714C5D1FCD61EA2B69D75E",33445,33445},
{"205.185.115.131","3091C6BEB2A993F1C6300C16549FABA67098FF3D62C6D253828B531470B53D68",53,53},
{"bg.tox.dcntrlzd.network","20AD2A54D70E827302CDF5F11D7C43FA0EC987042C36628E64B2B721A1426E36",33445,33445},
{"46.101.197.175","CD133B521159541FB1D326DE9850F5E56A6C724B5B8E5EB5CD8D950408E95707",33445,33445},
{"tox1.mf-net.eu","B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506",33445,33445},
{"tox2.mf-net.eu","70EA214FDE161E7432530605213F18F7427DC773E276B3E317A07531F548545F",33445,33445},
{"195.201.7.101","B84E865125B4EC4C368CD047C72BCE447644A2DC31EF75BD2CDA345BFD310107",33445,33445},
{"tox4.plastiras.org","836D1DA2BE12FE0E669334E437BE3FB02806F1528C2B2782113E0910C7711409",33445,33445},
{"gt.sot-te.ch","F4F4856F1A311049E0262E9E0A160610284B434F46299988A9CB42BD3D494618",33445,33445},
{"188.225.9.167","1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67",33445,33445},
{"122.116.39.151","5716530A10D362867C8E87EE1CD5362A233BAFBBA4CF47FA73B7CAD368BD5E6E",33445,33445},
{"195.123.208.139","534A589BA7427C631773D13083570F529238211893640C99D1507300F055FE73",33445,33445},
{"tox3.plastiras.org","4B031C96673B6FF123269FF18F2847E1909A8A04642BBECD0189AC8AEEADAF64",33445,33445},
{"104.225.141.59","933BA20B2E258B4C0D475B6DECE90C7E827FE83EFA9655414E7841251B19A72C",43334,43334},
{"139.162.110.188","F76A11284547163889DDC89A7738CF271797BF5E5E220643E97AD3C7E7903D55",33445,33445},
{"198.98.49.206","28DB44A3CEEE69146469855DFFE5F54DA567F5D65E03EFB1D38BBAEFF2553255",33445,33445},
{"172.105.109.31","D46E97CF995DC1820B92B7D899E152A217D36ABE22730FEA4B6BF1BFC06C617C",33445,33445},
{"ru.tox.dcntrlzd.network","DBB2E896990ECC383DA2E68A01CA148105E34F9B3B9356F2FE2B5096FDB62762",33445,33445},
{"91.146.66.26","B5E7DAC610DBDE55F359C7F8690B294C8E4FCEC4385DE9525DBFA5523EAD9D53",33445,33445},
{"tox01.ky0uraku.xyz","FD04EB03ABC5FC5266A93D37B4D6D6171C9931176DC68736629552D8EF0DE174",33445,33445},
{"tox02.ky0uraku.xyz","D3D6D7C0C7009FC75406B0A49E475996C8C4F8BCE1E6FC5967DE427F8F600527",33445,33445},
{"tox.plastiras.org","8E8B63299B3D520FB377FE5100E65E3322F7AE5B20A0ACED2981769FC5B43725",33445,33445},
{"kusoneko.moe","BE7ED53CD924813507BA711FD40386062E6DC6F790EFA122C78F7CDEEE4B6D1B",33445,33445},
{"tox2.plastiras.org","B6626D386BE7E3ACA107B46F48A5C4D522D29281750D44A0CBA6A2721E79C951",33445,33445},
{"172.104.215.182","DA2BD927E01CD05EBCC2574EBE5BEBB10FF59AE0B2105A7D1E2B40E49BB20239",33445,33445},
    { NULL, NULL, 0, 0 }
};

struct Node2 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes2[] = {
{ "tox.novg.net", "D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463", 33445, 33445 },
{ "bg.tox.dcntrlzd.network", "20AD2A54D70E827302CDF5F11D7C43FA0EC987042C36628E64B2B721A1426E36", 33445, 33445 },
{"91.219.59.156","8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832",33445,33445},
{"85.143.221.42","DA4E4ED4B697F2E9B000EEFE3A34B554ACD3F45F5C96EAEA2516DD7FF9AF7B43",33445,33445},
{"tox.initramfs.io","3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25",33445,33445},
{"144.217.167.73","7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C",33445,33445},
{"tox.abilinski.com","10C00EB250C3233E343E2AEBA07115A5C28920E9C8D29492F6D00B29049EDC7E",33445,33445},
{"tox.novg.net","D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463",33445,33445},
{"198.199.98.108","BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F",33445,33445},
{"tox.kurnevsky.net","82EF82BA33445A1F91A7DB27189ECFC0C013E06E3DA71F588ED692BED625EC23",33445,33445},
{"81.169.136.229","E0DB78116AC6500398DDBA2AEEF3220BB116384CAB714C5D1FCD61EA2B69D75E",33445,33445},
{"205.185.115.131","3091C6BEB2A993F1C6300C16549FABA67098FF3D62C6D253828B531470B53D68",53,53},
{"bg.tox.dcntrlzd.network","20AD2A54D70E827302CDF5F11D7C43FA0EC987042C36628E64B2B721A1426E36",33445,33445},
{"46.101.197.175","CD133B521159541FB1D326DE9850F5E56A6C724B5B8E5EB5CD8D950408E95707",33445,33445},
{"tox1.mf-net.eu","B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506",33445,33445},
{"tox2.mf-net.eu","70EA214FDE161E7432530605213F18F7427DC773E276B3E317A07531F548545F",33445,33445},
{"195.201.7.101","B84E865125B4EC4C368CD047C72BCE447644A2DC31EF75BD2CDA345BFD310107",33445,33445},
{"tox4.plastiras.org","836D1DA2BE12FE0E669334E437BE3FB02806F1528C2B2782113E0910C7711409",33445,33445},
{"gt.sot-te.ch","F4F4856F1A311049E0262E9E0A160610284B434F46299988A9CB42BD3D494618",33445,33445},
{"188.225.9.167","1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67",33445,33445},
{"122.116.39.151","5716530A10D362867C8E87EE1CD5362A233BAFBBA4CF47FA73B7CAD368BD5E6E",33445,33445},
{"195.123.208.139","534A589BA7427C631773D13083570F529238211893640C99D1507300F055FE73",33445,33445},
{"tox3.plastiras.org","4B031C96673B6FF123269FF18F2847E1909A8A04642BBECD0189AC8AEEADAF64",33445,33445},
{"104.225.141.59","933BA20B2E258B4C0D475B6DECE90C7E827FE83EFA9655414E7841251B19A72C",43334,43334},
{"139.162.110.188","F76A11284547163889DDC89A7738CF271797BF5E5E220643E97AD3C7E7903D55",33445,33445},
{"198.98.49.206","28DB44A3CEEE69146469855DFFE5F54DA567F5D65E03EFB1D38BBAEFF2553255",33445,33445},
{"172.105.109.31","D46E97CF995DC1820B92B7D899E152A217D36ABE22730FEA4B6BF1BFC06C617C",33445,33445},
{"ru.tox.dcntrlzd.network","DBB2E896990ECC383DA2E68A01CA148105E34F9B3B9356F2FE2B5096FDB62762",33445,33445},
{"91.146.66.26","B5E7DAC610DBDE55F359C7F8690B294C8E4FCEC4385DE9525DBFA5523EAD9D53",33445,33445},
{"tox01.ky0uraku.xyz","FD04EB03ABC5FC5266A93D37B4D6D6171C9931176DC68736629552D8EF0DE174",33445,33445},
{"tox02.ky0uraku.xyz","D3D6D7C0C7009FC75406B0A49E475996C8C4F8BCE1E6FC5967DE427F8F600527",33445,33445},
{"tox.plastiras.org","8E8B63299B3D520FB377FE5100E65E3322F7AE5B20A0ACED2981769FC5B43725",33445,33445},
{"kusoneko.moe","BE7ED53CD924813507BA711FD40386062E6DC6F790EFA122C78F7CDEEE4B6D1B",33445,33445},
{"tox2.plastiras.org","B6626D386BE7E3ACA107B46F48A5C4D522D29281750D44A0CBA6A2721E79C951",33445,33445},
{"172.104.215.182","DA2BD927E01CD05EBCC2574EBE5BEBB10FF59AE0B2105A7D1E2B40E49BB20239",33445,33445},
    { NULL, NULL, 0, 0 }
};


void dbg(int level, const char *fmt, ...)
{
    char *level_and_format = NULL;
    char *fmt_copy = NULL;

    if (fmt == NULL)
    {
        return;
    }

    if (strlen(fmt) < 1)
    {
        return;
    }

    if (!logfile)
    {
        return;
    }

    if ((level < 0) || (level > 9))
    {
        level = 0;
    }

    level_and_format = calloc(1, strlen(fmt) + 3 + 1);

    if (!level_and_format)
    {
        // dbg(9, stderr, "free:000a\n");
        return;
    }

    fmt_copy = level_and_format + 2;
    strcpy(fmt_copy, fmt);
    level_and_format[1] = ':';

    if (level == 0)
    {
        level_and_format[0] = 'E';
    }
    else if (level == 1)
    {
        level_and_format[0] = 'W';
    }
    else if (level == 2)
    {
        level_and_format[0] = 'I';
    }
    else
    {
        level_and_format[0] = 'D';
    }

    level_and_format[(strlen(fmt) + 2)] = '\0'; // '\0' or '\n'
    level_and_format[(strlen(fmt) + 3)] = '\0';
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t3 = time(NULL);
    struct tm tm3 = *localtime(&t3);
    char *level_and_format_2 = calloc(1, strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 7 + 1);
    level_and_format_2[0] = '\0';
    snprintf(level_and_format_2, (strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 7 + 1),
             "%04d-%02d-%02d %02d:%02d:%02d.%06ld:%s",
             tm3.tm_year + 1900, tm3.tm_mon + 1, tm3.tm_mday,
             tm3.tm_hour, tm3.tm_min, tm3.tm_sec, tv.tv_usec, level_and_format);

    if (level <= CURRENT_LOG_LEVEL)
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(logfile, level_and_format_2, ap);
        va_end(ap);
    }

    // dbg(9, "free:001\n");
    if (level_and_format)
    {
        // dbg(9, "free:001.a\n");
        free(level_and_format);
    }

    if (level_and_format_2)
    {
        free(level_and_format_2);
    }

    // dbg(9, "free:002\n");
}

void tox_log_cb__custom1(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func,
                        const char *message, void *user_data)
{
    dbg(9, "C-TOXCORE:1:%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}

void tox_log_cb__custom2(Tox *tox, TOX_LOG_LEVEL level, const char *file, uint32_t line, const char *func,
                        const char *message, void *user_data)
{
    dbg(9, "C-TOXCORE:2:%d:%s:%d:%s:%s\n", (int)level, file, (int)line, func, message);
}

static void hex_string_to_bin2(const char *hex_string, uint8_t *output) {
    size_t len = strlen(hex_string) / 2;
    size_t i = len;
    if (!output) {
        return;
    }

    const char *pos = hex_string;

    for (i = 0; i < len; ++i, pos += 2) {
        sscanf(pos, "%2hhx", &output[i]);
    }
}

static void update_savedata_file(const Tox *tox, int num)
{
    size_t size = tox_get_savedata_size(tox);
    char *savedata = calloc(1, size);
    tox_get_savedata(tox, (uint8_t *)savedata);

    FILE *f = NULL;
    if (num == 1)
    {
        f = fopen(savedata_filename1, "wb");
    }
    else
    {
        f = fopen(savedata_filename2, "wb");
    }
    fwrite(savedata, size, 1, f);
    fclose(f);

    free(savedata);
}

static Tox* tox_init(int num)
{
    Tox *tox = NULL;
    struct Tox_Options options;
    tox_options_default(&options);

    // ----- set options ------
    options.ipv6_enabled = false;
    options.local_discovery_enabled = true;
    options.hole_punching_enabled = true;
    options.udp_enabled = true;
    options.tcp_port = 0; // disable tcp relay function!
    // ----- set options ------

    FILE *f = NULL;
    if (num == 1)
    {
        f = fopen(savedata_filename1, "rb");
    }
    else
    {
        f = fopen(savedata_filename2, "rb");
    }

    if (f)
    {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *savedata = calloc(1, fsize);
        size_t dummy = fread(savedata, fsize, 1, f);

        if (dummy < 1)
        {
            dbg(0, "reading savedata failed\n");
        }

        fclose(f);
        options.savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
        options.savedata_data = savedata;
        options.savedata_length = fsize;
    }

    if (num == 1)
    {
        options.log_callback = tox_log_cb__custom1;
    }
    else
    {
        options.log_callback = tox_log_cb__custom2;
    }

    tox = tox_new(&options, NULL);
    return tox;
}

static bool tox_connect(Tox *tox, int num) {

    // dbg(9, "[%d]:bootstrapping ...\n", num);

    if (num == 1)
    {
        for (int i = 0; nodes1[i].ip; i++) {
            uint8_t *key = (uint8_t *)calloc(1, 100);
            hex_string_to_bin2(nodes1[i].key, key);
            if (!key) {
                return false; // Return because it will most likely fail again
            }

            tox_bootstrap(tox, nodes1[i].ip, nodes1[i].udp_port, key, NULL);
            if (nodes1[i].tcp_port != 0) {
                tox_add_tcp_relay(tox, nodes1[i].ip, nodes1[i].tcp_port, key, NULL);
            }
            free(key);
        }
    }
    else
    {
        for (int i = 0; nodes2[i].ip; i++) {
            uint8_t *key = (uint8_t *)calloc(1, 100);
            hex_string_to_bin2(nodes2[i].key, key);
            if (!key) {
                return false; // Return because it will most likely fail again
            }

            tox_bootstrap(tox, nodes2[i].ip, nodes2[i].udp_port, key, NULL);
            if (nodes2[i].tcp_port != 0) {
                tox_add_tcp_relay(tox, nodes2[i].ip, nodes2[i].tcp_port, key, NULL);
            }
            free(key);
        }
    }
    // dbg(9, "[%d]:bootstrapping done.\n", num);

    return true;
}

static void to_hex(char *out, uint8_t *in, int size) {
    while (size--) {
        if (*in >> 4 < 0xA) {
            *out++ = '0' + (*in >> 4);
        } else {
            *out++ = 'A' + (*in >> 4) - 0xA;
        }

        if ((*in & 0xf) < 0xA) {
            *out++ = '0' + (*in & 0xF);
        } else {
            *out++ = 'A' + (*in & 0xF) - 0xA;
        }
        in++;
    }
}

static void self_connection_change_callback(Tox *tox, TOX_CONNECTION status, void *userdata) {
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    switch (status) {
        case TOX_CONNECTION_NONE:
            dbg(9, "[%d]:Lost connection to the Tox network.\n", num);
            s_online[num] = 0;
            break;
        case TOX_CONNECTION_TCP:
            dbg(9, "[%d]:Connected using TCP.\n", num);
            s_online[num] = 1;
            break;
        case TOX_CONNECTION_UDP:
            dbg(9, "[%d]:Connected using UDP.\n", num);
            s_online[num] = 2;
            break;
    }
}

static void friend_connection_status_callback(Tox *tox, uint32_t friend_number, Tox_Connection connection_status,
        void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            dbg(9, "[%d]:Lost connection to friend %d\n", num, friend_number);
            f_online[num] = 0;
            break;
        case TOX_CONNECTION_TCP:
            dbg(9, "[%d]:Connected to friend %d using TCP\n", num, friend_number);
            f_online[num] = 1;
            break;
        case TOX_CONNECTION_UDP:
            dbg(9, "[%d]:Connected to friend %d using UDP\n", num, friend_number);
            f_online[num] = 2;
            break;
    }
}

static void friend_request_callback(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *userdata) {
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    TOX_ERR_FRIEND_ADD err;
    tox_friend_add_norequest(tox, public_key, &err);
    dbg(9, "[%d]:accepting friend request. res=%d\n", num, err);
}

static void group_invite_cb(Tox *tox, uint32_t friend_number, const uint8_t *invite_data, size_t length,
                                 const uint8_t *group_name, size_t group_name_length, void *userdata)
{
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    Tox_Err_Group_Invite_Accept error;
    tox_group_invite_accept(tox, friend_number, invite_data, length,
                                 (const uint8_t *)"tt", 2, NULL, 0,
                                 &error);

    dbg(9, "[%d]:tox_group_invite_accept:%d\n", num, error);
}

static void group_peer_join_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    f_join_other[num]++;
    dbg(9, "[%d]:Peer %d joined group %d other=%d\n", num, peer_id, group_number, f_join_other[num]);
}

static void group_self_join_cb(Tox *tox, uint32_t group_number, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    f_join_self[num]++;
    dbg(9, "[%d]:You joined group %d self=%d\n", num, group_number, f_join_self[num]);
}

static void group_join_fail_cb(Tox *tox, uint32_t group_number, Tox_Group_Join_Fail fail_type, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;
    dbg(9, "[%d]:Joining group %d failed. reason: %d\n", num, group_number, fail_type);
}

static void group_peer_exit_cb(Tox *tox, uint32_t group_number, uint32_t peer_id, Tox_Group_Exit_Type exit_type,
                                    const uint8_t *name, size_t name_length, const uint8_t *part_message, size_t length, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;
    dbg(9, "[%d]:Peer %d left group %d reason: %d\n", num, peer_id, group_number, exit_type);
}

static void t_toxav_call_cb(ToxAV *av, uint32_t friend_number, bool audio_enabled, bool video_enabled, void *user_data)
{
    av;
    uint8_t* unum = (uint8_t *)user_data;
    uint8_t num = *unum;
    dbg(9, "[%d]:t_toxav_call_cb\n", num);
    TOXAV_ERR_ANSWER error;
    toxav_answer(av, friend_number, 50, 1000, &error);
}

static void t_toxav_call_state_cb(ToxAV *av, uint32_t friend_number, uint32_t state, void *user_data)
{
    av;
    uint8_t* unum = (uint8_t *)user_data;
    uint8_t num = *unum;
    dbg(9, "[%d]:t_toxav_call_state_cb\n", num);
}

static void t_toxav_bit_rate_status_cb(ToxAV *av, uint32_t friend_number,
                                       uint32_t audio_bit_rate, uint32_t video_bit_rate,
                                       void *user_data)
{
    av;
    uint8_t* unum = (uint8_t *)user_data;
    uint8_t num = *unum;
    dbg(9, "[%d]:t_toxav_bit_rate_status_cb\n", num);
}

static void t_toxav_receive_video_frame_cb(ToxAV *av, uint32_t friend_number,
        uint16_t width, uint16_t height,
        uint8_t const *y, uint8_t const *u, uint8_t const *v,
        int32_t ystride, int32_t ustride, int32_t vstride,
        void *user_data)
{
    v_frames_rvcd++;
}

static void t_toxav_receive_video_frame_pts_cb(ToxAV *av, uint32_t friend_number,
        uint16_t width, uint16_t height,
        const uint8_t *y, const uint8_t *u, const uint8_t *v,
        int32_t ystride, int32_t ustride, int32_t vstride,
        void *user_data, uint64_t pts)
{
    av;
    uint8_t* unum = (uint8_t *)user_data;
    uint8_t num = *unum;

    v_frames_rvcd++;
    usleep(4 * 1000);
    uint8_t chk1 = *(y + (ystride * height) - 1);
    uint8_t chk2 = *(u + 1);
    uint8_t chk3 = *(v + 1);
    dbg(9, "[%d]:t_toxav_receive_video_frame_pts_cb %u %u %u\n", num, chk1, chk2, chk3);
}

static void t_toxav_receive_audio_frame_cb(ToxAV *av, uint32_t friend_number,
        int16_t const *pcm,
        size_t sample_count,
        uint8_t channels,
        uint32_t sampling_rate,
        void *user_data)
{
    a_frames_rvcd++;
}

static void t_toxav_receive_audio_frame_pts_cb(ToxAV *av, uint32_t friend_number,
        int16_t const *pcm,
        size_t sample_count,
        uint8_t channels,
        uint32_t sampling_rate,
        void *user_data,
        uint64_t pts)
{
    av;
    uint8_t* unum = (uint8_t *)user_data;
    uint8_t num = *unum;
    a_frames_rvcd++;
    dbg(9, "[%d]:t_toxav_receive_audio_frame_pts_cb\n", num);

}

static void t_toxav_call_comm_cb(ToxAV *av, uint32_t friend_number, TOXAV_CALL_COMM_INFO comm_value,
                                 int64_t comm_number, void *user_data)
{
}

static void set_cb(Tox *tox1, Tox *tox2, ToxAV *tox1av, ToxAV *tox2av)
{
    // ---------- CALLBACKS ----------
    tox_callback_self_connection_status(tox1, self_connection_change_callback);
    tox_callback_friend_connection_status(tox1, friend_connection_status_callback);
    tox_callback_friend_request(tox1, friend_request_callback);
    tox_callback_group_invite(tox1, group_invite_cb);
    tox_callback_group_peer_join(tox1, group_peer_join_cb);
    tox_callback_group_self_join(tox1, group_self_join_cb);
    tox_callback_group_peer_exit(tox1, group_peer_exit_cb);
    tox_callback_group_join_fail(tox1, group_join_fail_cb);

    toxav_callback_call(tox1av, t_toxav_call_cb, (void *)&s_num1);
    toxav_callback_call_state(tox1av, t_toxav_call_state_cb, (void *)&s_num1);
    toxav_callback_bit_rate_status(tox1av, t_toxav_bit_rate_status_cb, (void *)&s_num1);
    toxav_callback_video_receive_frame(tox1av, t_toxav_receive_video_frame_cb, (void *)&s_num1);
    toxav_callback_video_receive_frame_pts(tox1av, t_toxav_receive_video_frame_pts_cb, (void *)&s_num1);
    toxav_callback_audio_receive_frame(tox1av, t_toxav_receive_audio_frame_cb, (void *)&s_num1);
    toxav_callback_audio_receive_frame_pts(tox1av, t_toxav_receive_audio_frame_pts_cb, (void *)&s_num1);
    toxav_callback_call_comm(tox1av, t_toxav_call_comm_cb, (void *)&s_num1);

    tox_callback_self_connection_status(tox2, self_connection_change_callback);
    tox_callback_friend_connection_status(tox2, friend_connection_status_callback);
    tox_callback_friend_request(tox2, friend_request_callback);
    tox_callback_group_invite(tox2, group_invite_cb);
    tox_callback_group_peer_join(tox2, group_peer_join_cb);
    tox_callback_group_self_join(tox2, group_self_join_cb);
    tox_callback_group_peer_exit(tox2, group_peer_exit_cb);
    tox_callback_group_join_fail(tox2, group_join_fail_cb);

    toxav_callback_call(tox2av, t_toxav_call_cb, (void *)&s_num2);
    toxav_callback_call_state(tox2av, t_toxav_call_state_cb, (void *)&s_num2);
    toxav_callback_bit_rate_status(tox2av, t_toxav_bit_rate_status_cb, (void *)&s_num2);
    toxav_callback_video_receive_frame(tox2av, t_toxav_receive_video_frame_cb, (void *)&s_num2);
    toxav_callback_video_receive_frame_pts(tox2av, t_toxav_receive_video_frame_pts_cb, (void *)&s_num2);
    toxav_callback_audio_receive_frame(tox2av, t_toxav_receive_audio_frame_cb, (void *)&s_num2);
    toxav_callback_audio_receive_frame_pts(tox2av, t_toxav_receive_audio_frame_pts_cb, (void *)&s_num2);
    toxav_callback_call_comm(tox2av, t_toxav_call_comm_cb, (void *)&s_num2);
    // ---------- CALLBACKS ----------

}

uint32_t s_r(const uint32_t upper_bound)
{
    return randombytes_uniform(upper_bound);
}

uint32_t n_r(const uint32_t upper_bound)
{
    return rand() % upper_bound;
}

uint32_t generate_random_uint32()
{
#if 0
    // use libsodium function
    uint32_t r = randombytes_random();
    // dbg(9, "[%d]:random_sodium=%u\n", 0, r);
    return r;
#else
    // HINT: this is not perfectly randon, FIX ME!
    uint32_t r;
    r = rand();
    // dbg(9, "[%d]:random_glibc=%u\n", 0, r);
    return r;
#endif
}

static void print_stats(Tox *tox, int num)
{
    uint32_t num_friends = tox_self_get_friend_list_size(tox);
    uint32_t num_groups = tox_group_get_number_groups(tox);
    dbg(9, "[%d]:tox num_friends:%d\n", num, num_friends);
    dbg(9, "[%d]:tox num_groups:%d\n", num, num_groups);
}

static void del_savefile(int num)
{
    if (num == 1)
    {
        unlink(savedata_filename1);
    }
    else
    {
        unlink(savedata_filename2);
    }
}

void *thread_audio_av(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV audio Thread #%d: starting\n", id, id);

    usleep((generate_random_uint32() % 100) * 1000);

    while (toxav_audioiterate_thread_stop != 1)
    {
        toxav_audio_iterate(av);
        usleep((generate_random_uint32() % 7) * 1000);
    }

    dbg(9, "[%d]:ToxVideo:Clean audio thread exit\n", id);
    pthread_exit(0);
}

void *thread_video_av(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV video Thread #%d: starting\n", id, id);

    usleep((generate_random_uint32() % 100) * 1000);

    while (toxav_video_thread_stop != 1)
    {
        toxav_iterate(av);
        usleep((generate_random_uint32() % 9) * 1000);
    }

    dbg(9, "[%d]:ToxVideo:Clean video thread exit\n", id);
    pthread_exit(0);
}

void *thread_control_av_1(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV control Thread #%d: starting\n", 1, id);

    Toxav_Err_Call err;
    Toxav_Err_Call err2;
    bool res = 0;
    while (toxav_video_thread_stop != 1)
    {
        uint32_t call_or_not = generate_random_uint32() % 5;
        if (call_or_not == 4) {
            usleep((generate_random_uint32() % 2) * 1000 * 1000);
            dbg(9, "[%d]:ToxVideo:HANGUP=======================================>>>>>>>>>>>\n", 1);
            Toxav_Err_Call_Control error;
            toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
            usleep(2 * 100);
            toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
            usleep((generate_random_uint32() % 2) * 1000 * 1000);
        } else {
            res = toxav_call(av, 0, 20, 2000, &err2);
            dbg(9, "[%d]:ToxVideo:toxav_call:res=%d err=%d\n", 1, res, err2);
            usleep(10 * 1000);
            res = toxav_call(av, 0, 20, 2000, &err);
            dbg(9, "[%d]:ToxVideo:toxav_call:res=%d err=%d\n", 1, res, err);
            usleep(10 * 1000);
            res = toxav_call(av, 0, 20, 2000, &err);
            dbg(9, "[%d]:ToxVideo:toxav_call:res=%d err=%d\n", 1, res, err);

            dbg(9, "[%d]:ToxVideo:CALL***************************************>>>>>>>>>>>\n", 1);
            if (err2 == TOXAV_ERR_CALL_FRIEND_ALREADY_IN_CALL) {
            dbg(9, "[%d]:ToxVideo:HANGUP_______________________________________>>>>>>>>>>>\n", 1);
                Toxav_Err_Call_Control error;
                toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
                usleep(2 * 100);
                toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
            } else {
                usleep((generate_random_uint32() % 10) * 1000 * 1000);
            }
        }
    }

    dbg(9, "[%d]:ToxVideo:Clean control thread exit\n", 1);
    pthread_exit(0);
}

void *thread_control_av_2(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV control Thread #%d: starting\n", 2, id);

    while (toxav_video_thread_stop != 1)
    {
        usleep((generate_random_uint32() % 14) * 1000 * 1000);
        Toxav_Err_Call_Control error;
        toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
        usleep(2 * 100);
        toxav_call_control(av, 0, TOXAV_CALL_CONTROL_CANCEL, &error);
        usleep(30 * 1000);
        toxav_call_control(av, 0, TOXAV_CALL_CONTROL_RESUME, &error);
    }

    dbg(9, "[%d]:ToxVideo:Clean control thread exit\n", 2);
    pthread_exit(0);
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

void *thread_send_av_1(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV send Thread #%d: starting\n", 1, id);

    TOXAV_ERR_SEND_FRAME err;
    bool res = 0;
    int w = 1920;
    int h = 1080;
    int y_size = w * h;
    int u_size = (w/2) * (h/2);
    int v_size = (w/2) * (h/2);
    uint8_t *y = calloc(1, (w * h) * 3 / 2);
    uint8_t *u = y + (w * h);
    uint8_t *v = u + ((w/2) * (h/2));

    memset(y, 1, (w * h));
    memset(u, 2, ((w/2) * (h/2)));
    memset(v, 3, ((w/2) * (h/2)));

    while (toxav_video_thread_stop != 1)
    {
        rvbuf(y, y_size);
        rvbuf(u, u_size);
        rvbuf(v, v_size);
        usleep((generate_random_uint32() % 10) * 1000);
        bool v_sent = toxav_video_send_frame(av, 0, w, h, y, u, v, &err);
        if (v_sent)
        {
            v_frames_sent++;
        }
    }

    free(y);
    dbg(9, "[%d]:ToxVideo:Clean send thread exit\n", 1);
    pthread_exit(0);
}

void *thread_send_av_2(void *data)
{
    ToxAV *av = (ToxAV *) data;
    pthread_t id = pthread_self();
    dbg(9, "[%d]:AV send Thread #%d: starting\n", 2, id);

    TOXAV_ERR_SEND_FRAME err;
    bool res = 0;
    int w = 1920;
    int h = 1080;
    uint8_t *y = calloc(1, (w * h) * 3 / 2);
    uint8_t *u = y + (w * h);
    uint8_t *v = u + ((w/2) * (h/2));

    memset(y, 4, (w * h));
    memset(u, 5, ((w/2) * (h/2)));
    memset(v, 6, ((w/2) * (h/2)));

    while (toxav_video_thread_stop != 1)
    {
        memset(y, (generate_random_uint32() % 253) + 1, (w * h));
        usleep((generate_random_uint32() % 100) * 1000);
        bool v_sent = toxav_video_send_frame(av, 0, w, h, y, u, v, &err);
        if (v_sent)
        {
            v_frames_sent++;
        }
    }

    free(y);

    dbg(9, "[%d]:ToxVideo:Clean send thread exit\n", 2);
    pthread_exit(0);
}

int main(void)
{
    logfile = stdout;
    // logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IOLBF, 0);

    dbg(9, "--start--\n");

    // --- seed rand ---
    struct timeval time;
    gettimeofday(&time, NULL);
    srand((time.tv_sec * 1000) + (time.tv_usec / 1000));
    // --- seed rand ---

    dbg(9, "[%d]:remove old savefiles\n", 0);
    del_savefile(1);
    del_savefile(2);

    uint8_t num1 = 1;
    uint8_t num2 = 2;
    f_online[1] = 0;
    f_online[2] = 0;
    s_online[1] = 0;
    s_online[2] = 0;
    Tox *tox1 = tox_init(1);
    Tox *tox2 = tox_init(2);

    uint8_t public_key_bin1[TOX_ADDRESS_SIZE];
    char    public_key_str1[TOX_ADDRESS_SIZE * 2];
    tox_self_get_address(tox1, public_key_bin1);
    to_hex(public_key_str1, public_key_bin1, TOX_ADDRESS_SIZE);
    dbg(9, "[%d]:ID:1: %.*s\n", 1, TOX_ADDRESS_SIZE * 2, public_key_str1);

    uint8_t public_key_bin2[TOX_ADDRESS_SIZE];
    char    public_key_str2[TOX_ADDRESS_SIZE * 2];
    tox_self_get_address(tox2, public_key_bin2);
    to_hex(public_key_str2, public_key_bin2, TOX_ADDRESS_SIZE);
    dbg(9, "[%d]:ID:2: %.*s\n", 2, TOX_ADDRESS_SIZE * 2, public_key_str2);

    tox_self_set_name(tox1, "ABCtox1_789", strlen("ABCtox1_789"), NULL);
    tox_self_set_name(tox1, "UHGtox2_345", strlen("UHGtox2_345"), NULL);

    TOXAV_ERR_NEW rc;
    ToxAV *toxav1 = toxav_new(tox1, &rc);
    if (rc != TOXAV_ERR_NEW_OK)
    {
        dbg(9, "[%d]:Error at toxav_new: %d\n", 1, rc);
    }

    ToxAV *toxav2 = toxav_new(tox2, &rc);
    if (rc != TOXAV_ERR_NEW_OK)
    {
        dbg(9, "[%d]:Error at toxav_new: %d\n", 2, rc);
    }

    toxav_audio_iterate_seperation(toxav1, true);
    toxav_audio_iterate_seperation(toxav2, true);

    tox_connect(tox1, 1);
    tox_connect(tox2, 2);

    set_cb(tox1, tox2, toxav1, toxav2);

    tox_iterate(tox1, (void *)&num1);
<<<<<<< HEAD
    tox_iterate(tox2, (void *)&num2);
=======
    tox_iterate(tox2, (void *)&num1);
>>>>>>> cee098a21 (Revert "remove some tests")

    pthread_t tid[8];
    toxav_video_thread_stop = 0;
    toxav_audioiterate_thread_stop = 0;

    if (pthread_create(&(tid[0]), NULL, thread_video_av, (void *)toxav1) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[0], "t_toxav_v_ite1");
    }
    if (pthread_create(&(tid[1]), NULL, thread_audio_av, (void *)toxav1) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[1], "t_toxav_a_ite1");
    }

    if (pthread_create(&(tid[2]), NULL, thread_video_av, (void *)toxav2) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[2], "t_toxav_v_ite2");
    }
    if (pthread_create(&(tid[3]), NULL, thread_audio_av, (void *)toxav2) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[3], "t_toxav_a_ite2");
    }

    // ----------- wait for friends to come online -----------
    Tox_Err_Friend_Add err1;
    tox_friend_add(tox1, public_key_bin2, "1", 1, &err1);
    dbg(9, "[%d]:add friend res=%d\n", 1, err1);
    while (1 == 1) {
        tox_iterate(tox1, (void *)&num1);
        usleep(tox_iteration_interval(tox1) * 1000);
        tox_iterate(tox2, (void *)&num2);
        usleep(tox_iteration_interval(tox2) * 1000);
        if ((f_online[1] > 0) && (f_online[2] > 0))
        {
            break;
        }
    }
    dbg(9, "[%d]:friends online\n", 0);
    // ----------- wait for friends to come online -----------

    if (pthread_create(&(tid[4]), NULL, thread_control_av_1, (void *)toxav1) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[4], "t_toxav_v_ctrl1");
    }

    if (pthread_create(&(tid[5]), NULL, thread_control_av_2, (void *)toxav2) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[5], "t_toxav_v_ctrl2");
    }

    if (pthread_create(&(tid[6]), NULL, thread_send_av_1, (void *)toxav1) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[6], "t_toxav_v_snd1");
    }

    if (pthread_create(&(tid[7]), NULL, thread_send_av_2, (void *)toxav2) != 0)
    {
    }
    else
    {
        pthread_setname_np(tid[7], "t_toxav_v_snd2");
    }

#ifdef ENDLESS_RUN
    while (1==1) {
#endif
        for (long looper=0;looper<(80*60);looper++) {
            tox_iterate(tox1, (void *)&num1);
            usleep((generate_random_uint32() % 20) * 1000);
            tox_iterate(tox2, (void *)&num2);
            usleep((generate_random_uint32() % 20) * 1000);
        }
#ifdef ENDLESS_RUN
    }
#endif

    toxav_video_thread_stop = 1;
    toxav_audioiterate_thread_stop = 1;

    // wait for all threads to stop ---------
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    pthread_join(tid[4], NULL);
    pthread_join(tid[5], NULL);
    pthread_join(tid[6], NULL);
    pthread_join(tid[7], NULL);
    // wait for all threads to stop ---------

    usleep(1000 * 1000);

    toxav_kill(toxav1);
    toxav_kill(toxav2);

    tox_kill(tox1);
    tox_kill(tox2);

    dbg(9, "\n");
    dbg(9, "\n");
    dbg(9, "\n");
    dbg(9, "\n");
    dbg(9, "===============================================\n");
    dbg(9, "a_frames_rvcd: %ld v_frames_rvcd: %ld a_frames_sent: %ld v_frames_sent: %ld\n",
        a_frames_rvcd, v_frames_rvcd, a_frames_sent, v_frames_sent);
    dbg(9, "===============================================\n");
    dbg(9, "\n");
    dbg(9, "\n");

    dbg(9, "--END--\n");
    fclose(logfile);

    return 0;
} 

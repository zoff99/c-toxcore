

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

#include <tox/tox.h>

#define CURRENT_LOG_LEVEL 9 // 0 -> error, 1 -> warn, 2 -> info, 9 -> debug
const char *log_filename = "output.log";
const char *savedata_filename1 = "savedata1.tox";
const char *savedata_filename2 = "savedata2.tox";
FILE *logfile = NULL;
#define PARALLEL_FILES 5
uint8_t *key_bin_rec[PARALLEL_FILES];

const uint64_t totalf_size = 30L * 1024L * 1024L;
uint64_t cur_recv_pos = 0;
uint64_t cur_send_pos = 0;
uint32_t print_counter = 0;
uint8_t *send_file[PARALLEL_FILES];
uint8_t *recv_file[PARALLEL_FILES];

int f_online[3] = { 0, 0, 0};
int ft_fin[PARALLEL_FILES];

struct Node1 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes1[] = {
{ "tox1.mf-net.eu", "B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506", 33445, 3389 },
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

static uint32_t receive_filenum_to_filenum(uint32_t file_number)
{
    return (uint32_t)((file_number >> 16) - 1);
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

    uint8_t *savedata = NULL;
    if (f)
    {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        savedata = calloc(1, fsize);
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
    free(savedata);
    return tox;
}

static bool tox_connect(Tox *tox, int num) {

    dbg(9, "[%d]:bootstrapping ...\n", num);

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
    dbg(9, "[%d]:bootstrapping done.\n", num);

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
            break;
        case TOX_CONNECTION_TCP:
            dbg(9, "[%d]:Connected using TCP.\n", num);
            break;
        case TOX_CONNECTION_UDP:
            dbg(9, "[%d]:Connected using UDP.\n", num);
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

static void file_recv_chunk(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position, const uint8_t *data,
                       size_t length, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    // dbg(9, "[%d]: -> file_recv_chunk filenum=%d pos=%lu length=%d\n", num, filenumber, position, (int)length);

    if (data == NULL)
    {
        uint8_t key_bin_chunk[TOX_FILE_ID_LENGTH];
        char    key_str_chunk[TOX_FILE_ID_LENGTH * 2];
        Tox_Err_File_Get gfierr;
        tox_file_get_file_id(tox, 0, filenumber, key_bin_chunk, &gfierr);
        to_hex(key_str_chunk, key_bin_chunk, TOX_FILE_ID_LENGTH);
        dbg(9, "[%d]:ID:F: %.*s\n", num, TOX_FILE_ID_LENGTH * 2, key_str_chunk);
        dbg(9, "[%d]: -> file_recv_chunk FINISHED:RECEIVER filenum=%d pos=%lu length=%d\n", num, receive_filenum_to_filenum(filenumber), position, (int)length);
        
        dbg(9, "[%d]: -> file_recv_chunk FINISHED:RECEIVER:comparing files ...\n", num);
        uint8_t *send_file_tmp = send_file[receive_filenum_to_filenum(filenumber)];
        uint8_t *recv_file_tmp = recv_file[receive_filenum_to_filenum(filenumber)];
        uint64_t i = 0;
        while(i++ < totalf_size)
        {
            if ((uint8_t)*send_file_tmp != (uint8_t)*recv_file_tmp)
            {
                dbg(9, "[%d]:ERR: -> file_recv_chunk FINISHED:RECEIVER:byte does NOT match at position %d byte:%d byte:%d\n", num, i, (uint8_t)*send_file_tmp, (uint8_t)*recv_file_tmp);
                exit(3);
            }
            send_file_tmp++;
            recv_file_tmp++;
        }
        dbg(9, "[%d]: -> file_recv_chunk FINISHED:RECEIVER:comparing files ... DONE\n", num);
    }
    else
    {
        print_counter++;
        if ((print_counter % 770) == 0) // (770 * 1363) =~ 1MiB
        {
            dbg(9, "[%d]: -> file_recv_chunk filenum=%d pos=%lu length=%d\n", num, receive_filenum_to_filenum(filenumber), position, (int)length);
        }

        uint8_t key_bin_chunk[TOX_FILE_ID_LENGTH];
        char    key_str_chunk[TOX_FILE_ID_LENGTH * 2];
        memcpy(key_bin_chunk, data, TOX_FILE_ID_LENGTH);
        to_hex(key_str_chunk, key_bin_chunk, TOX_FILE_ID_LENGTH);
        // dbg(9, "[%d]:ID:C: %.*s\n", 1, TOX_FILE_ID_LENGTH * 2, key_str_chunk);

        cur_recv_pos = position;

        memcpy((recv_file[receive_filenum_to_filenum(filenumber)] + position), (data + TOX_FILE_ID_LENGTH), (length - TOX_FILE_ID_LENGTH));
    }

}

static void file_recv_control(Tox *tox, uint32_t friend_number, uint32_t file_number, Tox_File_Control control,
                               void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;
    dbg(9, "[%d]:file_recv_control. control=%d\n", num, control); // control "0" == "TOX_FILE_CONTROL_RESUME"
}

static void file_chunk_request(Tox *tox, uint32_t friend_number, uint32_t file_number, uint64_t position,
                                   size_t length, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;
    // dbg(9, "[%d]:file_chunk_request filenum=%d pos=%lu length=%d\n", num, file_number, position, (int)length);

    if (length == 0)
    {
        dbg(9, "[%d]:file_chunk_request FINISHED:SENDER filenum=%d pos=%lu length=%d\n", num, file_number, position, (int)length);
        ft_fin[file_number] = 1;
        return;
    }

    // dbg(9, "[%d]:file_chunk_request SENDER filenum=%d\n", num, file_number);

    char key_str_rec[TOX_FILE_ID_LENGTH * 2];
    to_hex(key_str_rec, key_bin_rec[file_number], TOX_FILE_ID_LENGTH);
    // dbg(9, "[%d]:ID:Sdr: %.*s\n", num, TOX_FILE_ID_LENGTH * 2, key_str_rec);


    uint8_t *f_data = calloc(1, length + TOX_FILE_ID_LENGTH);
    memcpy((f_data + TOX_FILE_ID_LENGTH), (send_file[file_number] + position), length);
    memcpy(f_data, key_bin_rec[file_number], TOX_FILE_ID_LENGTH);

    cur_send_pos = position;

    Tox_Err_File_Send_Chunk error;
    tox_file_send_chunk(tox, friend_number, file_number, position, f_data, (length + TOX_FILE_ID_LENGTH), &error);

    free(f_data);
}

static void file_receive(Tox *tox, uint32_t friend_number, uint32_t file_number, uint32_t kind, uint64_t filesize,
                             const uint8_t *filename, size_t filename_length, void *userdata)
{
    tox;
    uint8_t* unum = (uint8_t *)userdata;
    uint8_t num = *unum;

    dbg(9, "[%d]:file_receive:file_number=%d fnum2=%d\n", num, file_number, receive_filenum_to_filenum(file_number));

    char    key_str_rec[TOX_FILE_ID_LENGTH * 2];
    Tox_Err_File_Get gfierr;
    tox_file_get_file_id(tox, 0, file_number, key_bin_rec[receive_filenum_to_filenum(file_number)], &gfierr);
    to_hex(key_str_rec, key_bin_rec[receive_filenum_to_filenum(file_number)], TOX_FILE_ID_LENGTH);
    dbg(9, "[%d]:ID:R: %.*s\n", num, TOX_FILE_ID_LENGTH * 2, key_str_rec);

    dbg(9, "[%d]:file_receive:%s size=%lu kind=%d\n", num, filename, filesize, kind);

    if (filesize != totalf_size)
    {
        dbg(9, "[%d]:ERR:file_receive:%s size=%lu kind=%d does not match send size=%lu\n", num, filename, filesize, kind, (uint64_t)totalf_size);
        exit(2);
    }
    else
    {
        dbg(9, "[%d]:file_receive:%s size=%lu kind=%d DOES match send size=%lu\n", num, filename, filesize, kind, (uint64_t)totalf_size);
    }

    if (!recv_file[receive_filenum_to_filenum(file_number)])
    {
        recv_file[receive_filenum_to_filenum(file_number)] = calloc(1, filesize);
    }

    Tox_Err_File_Control error;
    tox_file_control(tox, friend_number, file_number, TOX_FILE_CONTROL_RESUME, &error);
}

static void set_cb(Tox *tox1, Tox *tox2)
{
    // ---------- CALLBACKS ----------
    tox_callback_self_connection_status(tox1, self_connection_change_callback);
    tox_callback_friend_connection_status(tox1, friend_connection_status_callback);
    tox_callback_friend_request(tox1, friend_request_callback);

    tox_callback_self_connection_status(tox2, self_connection_change_callback);
    tox_callback_friend_connection_status(tox2, friend_connection_status_callback);
    tox_callback_friend_request(tox2, friend_request_callback);


    tox_callback_file_recv_chunk(tox1, file_recv_chunk);
    tox_callback_file_recv_control(tox1, file_recv_control);
    tox_callback_file_recv(tox1, file_receive);

    tox_callback_file_recv_control(tox2, file_recv_control);
    tox_callback_file_chunk_request(tox2, file_chunk_request);
    // ---------- CALLBACKS ----------

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

int main(void)
{
    logfile = stdout;
    // logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IOLBF, 0);

    dbg(9, "--start--\n");

    dbg(9, "[%d]:remove old savefiles\n", 0);
    del_savefile(1);
    del_savefile(2);

    for (int k=0;k<PARALLEL_FILES;k++)
    {
        key_bin_rec[k] = calloc(1, TOX_FILE_ID_LENGTH);
        ft_fin[k] = 0;
    }

    uint8_t num1 = 1;
    uint8_t num2 = 2;
    f_online[1] = 0;
    f_online[2] = 0;
    Tox *tox1 = tox_init(1);
    Tox *tox2 = tox_init(2);

    uint8_t public_key_bin1[TOX_ADDRESS_SIZE];
    char public_key_str1[TOX_ADDRESS_SIZE * 2];
    tox_self_get_address(tox1, public_key_bin1);
    to_hex(public_key_str1, public_key_bin1, TOX_ADDRESS_SIZE);
    dbg(9, "[%d]:ID:1: %.*s\n", 1, TOX_ADDRESS_SIZE * 2, public_key_str1);

    uint8_t public_key_bin2[TOX_ADDRESS_SIZE];
    char public_key_str2[TOX_ADDRESS_SIZE * 2];
    tox_self_get_address(tox2, public_key_bin2);
    to_hex(public_key_str2, public_key_bin2, TOX_ADDRESS_SIZE);
    dbg(9, "[%d]:ID:2: %.*s\n", 2, TOX_ADDRESS_SIZE * 2, public_key_str2);

    tox_self_set_name(tox1, "ABCtox1_789", strlen("ABCtox1_789"), NULL);
    tox_self_set_name(tox1, "UHGtox2_345", strlen("UHGtox2_345"), NULL);

    print_stats(tox1, 1);
    print_stats(tox1, 2);

    tox_connect(tox1, 1);
    tox_connect(tox2, 2);

    set_cb(tox1, tox2);

    tox_iterate(tox1, (void *)&num1);
    tox_iterate(tox2, (void *)&num2);

    Tox_Err_Friend_Add err1;
    tox_friend_add(tox1, public_key_bin2, "1", 1, &err1);
    dbg(9, "[%d]:add friend res=%d\n", 1, err1);

    while (1 == 1) {
        tox_iterate(tox1, (void *)&num1);
        usleep(tox_iteration_interval(tox1)*1000);
        tox_iterate(tox2, (void *)&num2);
        usleep(tox_iteration_interval(tox2)*1000);
        if ((f_online[1] > 0) && (f_online[2] > 0))
        {
            break;
        }
    }

    dbg(9, "[%d]:friends online\n", 0);

    for (int k=0;k<PARALLEL_FILES;k++)
    {
        send_file[k] = calloc(1, (size_t)totalf_size);
    }

    dbg(9, "[%d]:generating random mem ...\n", 0);
    for (int k=0;k<PARALLEL_FILES;k++)
    {
        uint8_t *send_file_tmp = send_file[k];
        uint64_t i = 0;
        while(i++ < totalf_size)
        {
            uint8_t random_byte = (uint8_t)(rand() % 255);
            *send_file_tmp = random_byte;
            send_file_tmp++;
        }
    }
    dbg(9, "[%d]:generating random mem ... DONE\n", 0);

    for (int k=0;k<PARALLEL_FILES;k++)
    {
        char this_filename[300];
        snprintf(this_filename, 299, "Gentoo%d.exe", k);

        uint32_t fnum = tox_file_send(tox2, 0, TOX_FILE_KIND_FTV2, totalf_size, NULL, this_filename,
                                      strlen(this_filename), NULL);
        dbg(9, "[%d]:tox_file_send:file_number=%d\n", 0, fnum);

        uint8_t key_bin1[TOX_FILE_ID_LENGTH];
        char    key_str1[TOX_FILE_ID_LENGTH * 2];
        Tox_Err_File_Get gfierr1;
        tox_file_get_file_id(tox2, 0, fnum, key_bin1, &gfierr1);
        to_hex(key_str1, key_bin1, TOX_FILE_ID_LENGTH);
        dbg(9, "[%d]:ID:S: %.*s\n", 2, TOX_FILE_ID_LENGTH * 2, key_str1);
    }

    for (int i=0;i<10;i++) {
        tox_iterate(tox1, (void *)&num1);
        usleep(tox_iteration_interval(tox1)*1000);
        tox_iterate(tox2, (void *)&num2);
        usleep(tox_iteration_interval(tox2)*1000);
    }

    while (1 == 1) {
        tox_iterate(tox1, (void *)&num1);
        // usleep(tox_iteration_interval(tox1) * 1000);
        usleep(1*1000);
        tox_iterate(tox2, (void *)&num2);
        // usleep(tox_iteration_interval(tox2) * 1000);
        usleep(1*1000);
        int finished_fts = 0;
        for (int k=0;k<PARALLEL_FILES;k++)
        {
            if (ft_fin[k] == 1)
            {
                finished_fts++;
            }
        }
        if (finished_fts >= PARALLEL_FILES)
        {
            break;
        }
    }

    dbg(9, "[%d]:FTv2 ... DONE\n", 0);

    tox_kill(tox1);
    tox_kill(tox2);
    dbg(9, "[%d]:killed tox\n", 0);

    for (int k=0;k<PARALLEL_FILES;k++)
    {
        dbg(9, "[%d]:freeing buffers #%d\n", 0, k);
        free(send_file[k]);
        free(recv_file[k]);
        free(key_bin_rec[k]);
    }

    fclose(logfile);
    dbg(9, "[%d]:END\n", 0);
    return 0;
} 

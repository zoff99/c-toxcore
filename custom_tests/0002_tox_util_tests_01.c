

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
#include <tox/toxutil.h>

#define CURRENT_LOG_LEVEL 9 // 0 -> error, 1 -> warn, 2 -> info, 9 -> debug
const char *log_filename = "output.log";
const char *savedata_filename1 = "savedata1.tox";
const char *savedata_filename2 = "savedata2.tox";
FILE *logfile = NULL;
#define CLEAR(x) memset(&(x), 0, sizeof(x))

Tox *tox1 = NULL;
Tox *tox2 = NULL;

uint8_t s_num1 = 1;
uint8_t s_num2 = 2;

// #define USE_TOR 1
#define PROXY_PORT_TOR_DEFAULT 9050

int s_online[3] = { 0, 0, 0};
int f_online[3] = { 0, 0, 0};
int f_join_other[3] = { 0, 0, 0};
int f_join_self[3] = { 0, 0, 0};

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
    struct tm tm3;
    tm3 = *localtime_r(&t3, &tm3);
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

void rvbuf(uint8_t *buf, size_t size)
{
    for (int i=0; i < size; i++)
    {
        // random value 1..255
        *buf = (uint8_t)((n_r(255)) + 1);
        buf++;
    }
}

void bin2upHex(const uint8_t *bin, uint32_t bin_size, char *hex, uint32_t hex_size)
{
    sodium_bin2hex(hex, hex_size, bin, bin_size);

    for (size_t i = 0; i < hex_size - 1; i++) {
        hex[i] = toupper(hex[i]);
    }
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

unsigned int char_to_int(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)c - '0';
    }

    if (c >= 'A' && c <= 'F') {
        return 10 + (uint8_t)c - 'A';
    }

    if (c >= 'a' && c <= 'f') {
        return 10 + (uint8_t)c - 'a';
    }

    return -1;
}

uint8_t *tox_pubkey_hex_string_to_bin2(const char *hex_string)
{
    size_t len = tox_public_key_size();
    uint8_t *val = calloc(1, len);

    for (size_t i = 0; i < len; ++i) {
        val[i] = (16 * char_to_int(hex_string[2 * i])) + (char_to_int(hex_string[2 * i + 1]));
    }

    return val;
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

    tox = tox_utils_new(&options, NULL);
    return tox;
}

static bool tox_connect(Tox *tox, int num) {

    // dbg(9, "[%d]:bootstrapping ...\n", num);

    if (num == 1)
    {
        for (int i = 0; nodes1[i].ip; i++) {
            uint8_t *key = (uint8_t *)calloc(1, 100);
            hex_string_to_bin2(nodes1[i].key, key);
            tox_bootstrap(tox, nodes1[i].ip, nodes1[i].udp_port, key, NULL);
            free(key);
        }
    }
    else
    {
        for (int i = 0; nodes2[i].ip; i++) {
            uint8_t *key = (uint8_t *)calloc(1, 100);
            hex_string_to_bin2(nodes2[i].key, key);
            tox_bootstrap(tox, nodes2[i].ip, nodes2[i].udp_port, key, NULL);
            free(key);
        }
    }
    // dbg(9, "[%d]:bootstrapping done.\n", num);

    return true;
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

static void friend_sync_message_v2(Tox *tox, uint32_t friend_number,
                        const uint8_t *raw_message, size_t raw_message_len)
{
    dbg(9, "[%d]: friend_sync_message_v2\n", (tox == tox1) ? 1 : 2);
}

static void friend_read_receipt_message_v2(Tox *tox, uint32_t friend_number,
                        const uint8_t *raw_message, size_t raw_message_len)
{
    dbg(9, "[%d]: friend_read_receipt_message_v2\n", (tox == tox1) ? 1 : 2);
}

static void friend_message_v2(Tox *tox, uint32_t friend_number,
                        const uint8_t *raw_message, size_t raw_message_len)
{
    dbg(9, "[%d]: friend_message_v2\n", (tox == tox1) ? 1 : 2);

    // now get the real data from msgV2 buffer
    uint8_t *message_text = calloc(1, raw_message_len);

    if (message_text)
    {
        uint32_t ts_sec = tox_messagev2_get_ts_sec(raw_message);
        uint16_t ts_ms = tox_messagev2_get_ts_ms(raw_message);
        uint32_t text_length = 0;
        dbg(9, "friend_message_v2:%p %d\n", (char *)message_text, (int)message_text[0]);
        bool res = tox_messagev2_get_message_text(raw_message,
                   (uint32_t)raw_message_len,
                   (bool)false, (uint32_t)0,
                   message_text, &text_length);
        dbg(9, "friend_message_v2:res=%d:%d:%d:len=%d:%s\n", (int)res, (int)ts_sec, (int)ts_ms,
                (int)raw_message_len,
                (char *)raw_message);
        dbg(9, "friend_message_v2:fn=%d res=%d msg=%s\n", (int)friend_number, (int)res,
            (char *)message_text);
        // send msgV2 receipt
        uint8_t *msgid_buffer = calloc(1, TOX_PUBLIC_KEY_SIZE + 1);

        if (msgid_buffer)
        {
            memset(msgid_buffer, 0, TOX_PUBLIC_KEY_SIZE + 1);
            bool res2 = tox_messagev2_get_message_id(raw_message, msgid_buffer);
            uint32_t ts_sec22 = (uint32_t)time(NULL);
            tox_util_friend_send_msg_receipt_v2(tox, (uint32_t) friend_number, msgid_buffer, ts_sec22);
            free(msgid_buffer);
        }

        free(message_text);
    }
}


static void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                       size_t length, void *user_data)
{
    dbg(9, "Message: %s\n", message);
}

static void set_cb(Tox *tox1, Tox *tox2)
{
    // ---------- CALLBACKS ----------

    tox_utils_callback_self_connection_status(tox1, self_connection_change_callback);
    tox_callback_self_connection_status(tox1, tox_utils_self_connection_status_cb);
    tox_utils_callback_friend_connection_status(tox1, friend_connection_status_callback);
    tox_callback_friend_connection_status(tox1, tox_utils_friend_connection_status_cb);
    tox_callback_friend_lossless_packet(tox1, tox_utils_friend_lossless_packet_cb);
    tox_callback_file_recv_control(tox1, tox_utils_file_recv_control_cb);
    tox_callback_file_chunk_request(tox1, tox_utils_file_chunk_request_cb);
    tox_callback_file_recv(tox1, tox_utils_file_recv_cb);
    tox_callback_file_recv_chunk(tox1, tox_utils_file_recv_chunk_cb);

    tox_callback_friend_request(tox1, friend_request_callback);
    tox_callback_friend_message(tox1, friend_message_cb);
    tox_utils_callback_friend_message_v2(tox1, friend_message_v2);
    tox_utils_callback_friend_sync_message_v2(tox1, friend_sync_message_v2);
    tox_utils_callback_friend_read_receipt_message_v2(tox1, friend_read_receipt_message_v2);



    tox_utils_callback_self_connection_status(tox2, self_connection_change_callback);
    tox_callback_self_connection_status(tox2, tox_utils_self_connection_status_cb);
    tox_utils_callback_friend_connection_status(tox2, friend_connection_status_callback);
    tox_callback_friend_connection_status(tox2, tox_utils_friend_connection_status_cb);
    tox_callback_friend_lossless_packet(tox2, tox_utils_friend_lossless_packet_cb);
    tox_callback_file_recv_control(tox2, tox_utils_file_recv_control_cb);
    tox_callback_file_chunk_request(tox2, tox_utils_file_chunk_request_cb);
    tox_callback_file_recv(tox2, tox_utils_file_recv_cb);
    tox_callback_file_recv_chunk(tox2, tox_utils_file_recv_chunk_cb);

    tox_callback_friend_request(tox2, friend_request_callback);
    tox_callback_friend_message(tox2, friend_message_cb);
    tox_utils_callback_friend_message_v2(tox2, friend_message_v2);
    tox_utils_callback_friend_sync_message_v2(tox2, friend_sync_message_v2);
    tox_utils_callback_friend_read_receipt_message_v2(tox2, friend_read_receipt_message_v2);
    // ---------- CALLBACKS ----------
}

time_t get_unix_time(void)
{
    return time(NULL);
}

int main(void)
{
    logfile = stdout;
    setvbuf(logfile, NULL, _IOLBF, 0);

    dbg(9, "--start--\n");

    // --- seed rand ---
    struct timeval time;
    gettimeofday(&time, NULL);
    srand((time.tv_sec * 1000) + (time.tv_usec / 1000));
    // --- seed rand ---

    uint8_t num1 = 1;
    uint8_t num2 = 2;
    f_online[1] = 0;
    f_online[2] = 0;
    s_online[1] = 0;
    s_online[2] = 0;
    tox1 = tox_init(1);
    tox2 = tox_init(2);


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

    tox_connect(tox1, 1);
    tox_connect(tox2, 2);

    set_cb(tox1, tox2);

    tox_iterate(tox1, (void *)&num1);
    tox_iterate(tox2, (void *)&num2);

    // ----------- wait for friends to come online -----------
    Tox_Err_Friend_Add err1;
    tox_friend_add(tox1, public_key_bin2, "1", 1, &err1);
    dbg(9, "[%d]:add friend res=%d\n", 1, err1);
    while (1 == 1) {
        tox_iterate(tox1, (void *)&num1);
        usleep(tox_iteration_interval(tox1) * 1000);
        tox_iterate(tox2, (void *)&num2);
        usleep(tox_iteration_interval(tox2) * 1000);
        if ((f_online[1] > 1) && (f_online[2] > 1) && (s_online[1] > 1) && (s_online[2] > 1))
        {
            break;
        }
    }
    dbg(9, "[%d]:friends online via UDP and self toxes online via UDP\n", 0);
    // ----------- wait for friends to come online -----------


    for(int i=0;i<20;i++)
    {
        tox_iterate(tox1, (void *)&num1);
        usleep(tox_iteration_interval(tox1) * 1000);
        tox_iterate(tox2, (void *)&num2);
        usleep(tox_iteration_interval(tox2) * 1000);
    }

    uint64_t sc = tox_self_get_capabilities();
    uint64_t fc1 = tox_friend_get_capabilities(tox1, 0);
    uint64_t fc2 = tox_friend_get_capabilities(tox2, 0);
    dbg(9, "capabilities: %ld %ld %ld\n", (long)sc, (long)fc1, (long)fc2);

    // for(int f=0;f<2;f++)
    //{
        tox_friend_send_message(tox1, 0, TOX_MESSAGE_TYPE_NORMAL, "hello", strlen("hello"), NULL);

        size_t fsize = 1200;
        uint32_t *rawMsgData = (uint32_t *)calloc(1, fsize);
        uint32_t tox_public_key_hex_size = tox_public_key_size() * 2;

        uint8_t *pubKeyHex = calloc(1, (tox_public_key_size() * 2) + 1);
        memset(pubKeyHex, 65, tox_public_key_hex_size);

        uint32_t rawMsgSize2 = tox_messagev2_size(fsize, TOX_FILE_KIND_MESSAGEV2_SYNC, 0);
        uint8_t *raw_message2 = calloc(1, rawMsgSize2);
        uint8_t *msgid2 = calloc(1, tox_public_key_size());
        uint8_t *pubKeyBin = tox_pubkey_hex_string_to_bin2(pubKeyHex);

        //tox_messagev2_sync_wrap(fsize, pubKeyBin, TOX_FILE_KIND_MESSAGEV2_ANSWER,
        //                        rawMsgData, 665, 987, raw_message2, msgid2);

        tox_messagev2_sync_wrap(fsize, pubKeyBin, TOX_FILE_KIND_MESSAGEV2_SEND,
                                rawMsgData, 987, 775, raw_message2, msgid2);

        char msgid2_str[tox_public_key_hex_size + 1];
        CLEAR(msgid2_str);
        bin2upHex(msgid2, tox_public_key_size(), msgid2_str, tox_public_key_hex_size + 1);

        char msgid_orig_str[tox_public_key_hex_size + 1];
        CLEAR(msgid_orig_str);
        bin2upHex(rawMsgData, tox_public_key_size(), msgid_orig_str, tox_public_key_hex_size + 1);

        dbg(9, "send_sync_msg_single:msgid2=%s msgid_orig=%s\n", msgid2_str, msgid_orig_str);

        uint32_t ts_sec = (uint32_t) get_unix_time();

        uint8_t *raw_message_back = calloc(1, 10000);
        uint32_t raw_msg_len_back;
        uint8_t *msgid_back = calloc(1, 10000);
        TOX_ERR_FRIEND_SEND_MESSAGE err4;
        char *msg2 = "3098409234iu9e0wi4rposekrpiok309k90w3k9kf309wf";
        int64_t res11 = tox_util_friend_send_message_v2(tox1, 0, TOX_MESSAGE_TYPE_NORMAL, ts_sec, (const uint8_t *) msg2, strlen(msg2),
                                        raw_message_back, &raw_msg_len_back, msgid_back, &err4);
        dbg(9, "friend_send_message_v2: res=%ld; error=%d\n", (long)res11, err4);

        for(int i=0;i<20;i++)
        {
            tox_iterate(tox1, (void *)&num1);
            usleep(tox_iteration_interval(tox1) * 1000);
            tox_iterate(tox2, (void *)&num2);
            usleep(tox_iteration_interval(tox2) * 1000);
        }

        free(raw_message_back);
        free(msgid_back);

        TOX_ERR_FRIEND_SEND_MESSAGE error;
        bool res2 = tox_util_friend_send_sync_message_v2(tox1, 0, raw_message2, rawMsgSize2, &error);
        dbg(9, "send_sync_msg_single: send_sync_msg res=%d; error=%d\n", (int)res2, error);

        for(int i=0;i<20;i++)
        {
            tox_iterate(tox1, (void *)&num1);
            usleep(tox_iteration_interval(tox1) * 1000);
            tox_iterate(tox2, (void *)&num2);
            usleep(tox_iteration_interval(tox2) * 1000);
        }


/*
  need to test these:

tox_util_friend_resend_message_v2
tox_util_friend_send_sync_message_v2
tox_util_friend_send_message_v2

*/
        free(rawMsgData);
        free(raw_message2);
        free(pubKeyBin);
        free(msgid2);
        free(pubKeyHex);


    //}

    tox_utils_kill(tox1);
    tox_utils_kill(tox2);


    dbg(9, "--END--\n");
    fclose(logfile);

    return 0;
} 

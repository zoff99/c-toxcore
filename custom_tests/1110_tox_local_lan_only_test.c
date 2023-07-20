

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

// #define USE_TOR 1
#define PROXY_PORT_TOR_DEFAULT 9050

int s_online[3] = { 0, 0, 0};
int f_online[3] = { 0, 0, 0};
int f_join_other[3] = { 0, 0, 0};
int f_join_self[3] = { 0, 0, 0};

int toxav_video_thread_stop = 0;
int toxav_audioiterate_thread_stop = 0;

long a_frames_rvcd = 0;
long v_frames_rvcd = 0;
long a_frames_sent = 0;
long v_frames_sent = 0;
uint64_t last_vframe_recv_ts = 0;
uint64_t last_vframe_recv_pts = 0;
uint64_t last_aframe_recv_ts = 0;
uint64_t last_aframe_recv_pts = 0;

struct Node1 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes1[] = {
{ "127.0.2.2", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBBBBBBBB", 33445, 3389 },
    { NULL, NULL, 0, 0 }
};

struct Node2 {
    char *ip;
    char *key;
    uint16_t udp_port;
    uint16_t tcp_port;
} nodes2[] = {
{ "127.0.2.2", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABBBBBBBBBBBBBBBBBBBBBBBBBB", 33445, 3389 },
    { NULL, NULL, 0, 0 }
};


// gives a counter value that increaes every millisecond
static uint64_t current_time_monotonic_default2()
{
    uint64_t time = 0;
    struct timespec clock_mono;
    clock_gettime(CLOCK_MONOTONIC, &clock_mono);
    time = 1000ULL * clock_mono.tv_sec + (clock_mono.tv_nsec / 1000000ULL);
    return time;
}

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

static void set_cb(Tox *tox1, Tox *tox2)
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

    tox_callback_self_connection_status(tox2, self_connection_change_callback);
    tox_callback_friend_connection_status(tox2, friend_connection_status_callback);
    tox_callback_friend_request(tox2, friend_request_callback);
    tox_callback_group_invite(tox2, group_invite_cb);
    tox_callback_group_peer_join(tox2, group_peer_join_cb);
    tox_callback_group_self_join(tox2, group_self_join_cb);
    tox_callback_group_peer_exit(tox2, group_peer_exit_cb);
    tox_callback_group_join_fail(tox2, group_join_fail_cb);
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

    // HINT: force to use only UDP
    // ** // tox_set_force_udp_only_mode(true);

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
        if ((f_online[1] > 0) && (f_online[2] > 0))
        {
            break;
        }
    }
    dbg(9, "[%d]:friends online\n", 0);
    // ----------- wait for friends to come online -----------

    tox_kill(tox1);
    tox_kill(tox2);

    dbg(9, "--END--\n");
    fclose(logfile);

    return 0;
} 

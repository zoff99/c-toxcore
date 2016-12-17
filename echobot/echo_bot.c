#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>


#include <unistd.h>

#include <sodium/utils.h>
#include <tox/tox.h>

typedef struct DHT_node {
    const char *ip;
    uint16_t port;
    const char key_hex[TOX_PUBLIC_KEY_SIZE*2 + 1];
    unsigned char key_bin[TOX_PUBLIC_KEY_SIZE];
} DHT_node;

#define MAX_AVATAR_FILE_SIZE 65536
#define TOXIC_MAX_NAME_LENGTH 32   /* Must be <= TOX_MAX_NAME_LENGTH */

define KiB 1024
#define MiB 1048576       /* 1024^2 */
#define GiB 1073741824    /* 1024^3 */

#define MAX_FILES 32

typedef enum FILE_TRANSFER_STATE {
    FILE_TRANSFER_INACTIVE,
    FILE_TRANSFER_PAUSED,
    FILE_TRANSFER_PENDING,
    FILE_TRANSFER_STARTED,
} FILE_TRANSFER_STATE;

typedef enum FILE_TRANSFER_DIRECTION {
    FILE_TRANSFER_SEND,
    FILE_TRANSFER_RECV
} FILE_TRANSFER_DIRECTION;

struct FileTransfer {
    FILE *file;
    FILE_TRANSFER_STATE state;
    FILE_TRANSFER_DIRECTION direction;
    uint8_t file_type;
    char file_name[TOX_MAX_FILENAME_LENGTH + 1];
    char file_path[PATH_MAX + 1];    /* Not used by senders */
    double   bps;
    uint32_t filenum;
    uint32_t friendnum;
    size_t   index;
    uint64_t file_size;
    uint64_t position;
    time_t   last_line_progress;   /* The last time we updated the progress bar */
    time_t   last_keep_alive;  /* The last time we sent or received data */
    uint32_t line_id;
    uint8_t  file_id[TOX_FILE_ID_LENGTH];
};


struct LastOnline {
    uint64_t last_on;
    struct tm tm;
    char hour_min_str[TIME_STR_SIZE];    /* holds 12/24-hour time string e.g. "10:43 PM" */
};

struct GroupChatInvite {
    char *key;
    uint16_t length;
    uint8_t type;
    bool pending;
};

typedef struct {
    char name[TOXIC_MAX_NAME_LENGTH + 1];
    int namelength;
    char statusmsg[TOX_MAX_STATUS_MESSAGE_LENGTH + 1];
    size_t statusmsg_len;
    char pub_key[TOX_PUBLIC_KEY_SIZE];
    uint32_t num;
    int chatwin;
    bool active;
    TOX_CONNECTION connection_status;
    bool is_typing;
    bool logging_on;    /* saves preference for friend irrespective of global settings */
    uint8_t status;
    struct LastOnline last_online;
    struct FileTransfer file_receiver[MAX_FILES];
    struct FileTransfer file_sender[MAX_FILES];
} ToxicFriend;

typedef struct {
    char name[TOXIC_MAX_NAME_LENGTH + 1];
    int namelength;
    char pub_key[TOX_PUBLIC_KEY_SIZE];
    uint32_t num;
    bool active;
    uint64_t last_on;
} BlockedFriend;

typedef struct {
    int num_selected;
    size_t num_friends;
    size_t num_online;
    size_t max_idx;    /* 1 + the index of the last friend in list */
    uint32_t *index;
    ToxicFriend *list;
} FriendsList;


static struct Avatar {
    char name[TOX_MAX_FILENAME_LENGTH + 1];
    size_t name_len;
    char path[PATH_MAX + 1];
    size_t path_len;
    off_t size;
} Avatar;

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";
const char *log_filename = "echobot.log";
FILE *logfile = NULL;

Tox *create_tox()
{
    Tox *tox;

    struct Tox_Options options;

    tox_options_default(&options);

    FILE *f = fopen(savedata_filename, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *savedata = malloc(fsize);

        fread(savedata, fsize, 1, f);
        fclose(f);

        options.savedata_type = TOX_SAVEDATA_TYPE_TOX_SAVE;
        options.savedata_data = savedata;
        options.savedata_length = fsize;

        tox = tox_new(&options, NULL);

        free(savedata);
    } else {
        tox = tox_new(&options, NULL);
    }

    return tox;
}

void update_savedata_file(const Tox *tox)
{
    size_t size = tox_get_savedata_size(tox);
    char *savedata = malloc(size);
    tox_get_savedata(tox, savedata);

    FILE *f = fopen(savedata_tmp_filename, "wb");
    fwrite(savedata, size, 1, f);
    fclose(f);

    rename(savedata_tmp_filename, savedata_filename);

    free(savedata);
}

void bootstrap(Tox *tox)
{
    DHT_node nodes[] =
    {
        {"178.62.250.138",             33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"2a03:b0c0:2:d0::16:1",       33445, "788236D34978D1D5BD822F0A5BEBD2C53C64CC31CD3149350EE27D4D9A2F9B6B", {0}},
        {"tox.zodiaclabs.org",         33445, "A09162D68618E742FFBCA1C2C70385E6679604B2D80EA6E84AD0996A1AC8A074", {0}},
        {"163.172.136.118",            33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
        {"2001:bc8:4400:2100::1c:50f", 33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B", {0}},
        {"128.199.199.197",            33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", {0}},
        {"2400:6180:0:d0::17a:a001",   33445, "B05C8869DBB4EDDD308F43C1A974A20A725A36EACCA123862FDE9945BF9D3E09", {0}},
        {"biribiri.org",               33445, "F404ABAA1C99A9D37D61AB54898F56793E1DEF8BD46B1038B9D822E8460FAB67", {0}}
    };

    for (size_t i = 0; i < sizeof(nodes)/sizeof(DHT_node); i ++) {
        sodium_hex2bin(nodes[i].key_bin, sizeof(nodes[i].key_bin),
                       nodes[i].key_hex, sizeof(nodes[i].key_hex)-1, NULL, NULL, NULL);
        tox_bootstrap(tox, nodes[i].ip, nodes[i].port, nodes[i].key_bin, NULL);
    }
}

void print_tox_id(Tox *tox)
{
    uint8_t tox_id_bin[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox, tox_id_bin);

    char tox_id_hex[TOX_ADDRESS_SIZE*2 + 1];
    sodium_bin2hex(tox_id_hex, sizeof(tox_id_hex), tox_id_bin, sizeof(tox_id_bin));

    for (size_t i = 0; i < sizeof(tox_id_hex)-1; i ++) {
        tox_id_hex[i] = toupper(tox_id_hex[i]);
    }

    if (logfile)
    {
        printf("--MyToxID--:%s\n", tox_id_hex);
        fprintf(logfile, "--MyToxID--:%s\n", tox_id_hex);
        fsync(logfile);
    }
}

void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *user_data)
{
    tox_friend_add_norequest(tox, public_key, NULL);

    update_savedata_file(tox);
}

void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                   size_t length, void *user_data)
{
    tox_friend_send_message(tox, friend_number, type, message, length, NULL);
}

void on_file_chunk_request(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position,
                           size_t length, void *userdata)
{

    if (ft->file_type == TOX_FILE_KIND_AVATAR)
    {
        on_avatar_chunk_request(m, ft, position, length);
        return;
    }

}

void on_avatar_chunk_request(Tox *m, struct FileTransfer *ft, uint64_t position, size_t length)
{
    if (ft->state != FILE_TRANSFER_STARTED)
        return;

    if (length == 0) {
        close_file_transfer(NULL, m, ft, -1, NULL, silent);
        return;
    }

    if (ft->file == NULL) {
        close_file_transfer(NULL, m, ft, TOX_FILE_CONTROL_CANCEL, NULL, silent);
        return;
    }

    if (ft->position != position) {
        if (fseek(ft->file, position, SEEK_SET) == -1) {
            close_file_transfer(NULL, m, ft, TOX_FILE_CONTROL_CANCEL, NULL, silent);
            return;
        }

        ft->position = position;
    }

    uint8_t send_data[length];
    size_t send_length = fread(send_data, 1, sizeof(send_data), ft->file);

    if (send_length != length) {
        close_file_transfer(NULL, m, ft, TOX_FILE_CONTROL_CANCEL, NULL, silent);
        return;
    }
    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(m, ft->friendnum, ft->filenum, position, send_data, send_length, &err);

    if (err != TOX_ERR_FILE_SEND_CHUNK_OK)
        fprintf(stderr, "tox_file_send_chunk failed in avatar callback (error %d)\n", err);

    ft->position += send_length;
    ft->last_keep_alive = get_unix_time();
}


void self_connection_status_cb(Tox *tox, TOX_CONNECTION connection_status, void *user_data)
{
    switch (connection_status) {
        case TOX_CONNECTION_NONE:
            printf("Offline\n");
            break;
        case TOX_CONNECTION_TCP:
            printf("Online, using TCP\n");
            break;
        case TOX_CONNECTION_UDP:
            printf("Online, using UDP\n");
            break;
    }
}


static struct FileTransfer *new_file_sender(uint32_t friendnum, uint32_t filenum, uint8_t type)
{
    size_t i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        // TODO
        struct FileTransfer *ft = &Friends.list[friendnum].file_sender[i];

        if (ft->state == FILE_TRANSFER_INACTIVE) {
            memset(ft, 0, sizeof(struct FileTransfer));
            ft->index = i;
            ft->friendnum = friendnum;
            ft->filenum = filenum;
            ft->file_type = type;
            ft->last_keep_alive = get_unix_time();
            ft->state = FILE_TRANSFER_PENDING;
            ft->direction = FILE_TRANSFER_SEND;
            return ft;
        }
    }

    return NULL;
}



static struct FileTransfer *new_file_receiver(uint32_t friendnum, uint32_t filenum, uint8_t type)
{
    size_t i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        // TODO
        struct FileTransfer *ft = &Friends.list[friendnum].file_receiver[i];

        if (ft->state == FILE_TRANSFER_INACTIVE) {
            memset(ft, 0, sizeof(struct FileTransfer));
            ft->index = i;
            ft->friendnum = friendnum;
            ft->filenum = filenum;
            ft->file_type = type;
            ft->last_keep_alive = get_unix_time();
            ft->state = FILE_TRANSFER_PENDING;
            ft->direction = FILE_TRANSFER_RECV;
            return ft;
        }
    }

    return NULL;
}


struct FileTransfer *new_file_transfer(uint32_t friendnum, uint32_t filenum,
                                       FILE_TRANSFER_DIRECTION direction, uint8_t type)
{
    if (direction == FILE_TRANSFER_RECV)
    {
        return new_file_receiver(friendnum, filenum, type);
    }

    if (direction == FILE_TRANSFER_SEND)
    {
        return new_file_sender(friendnum, filenum, type);
    }

    return NULL;
}


int avatar_send(Tox *m, uint32_t friendnum)
{
    TOX_ERR_FILE_SEND err;
    uint32_t filenum = tox_file_send(m, friendnum, TOX_FILE_KIND_AVATAR, (size_t) Avatar.size,
                                     NULL, (uint8_t *) Avatar.name, Avatar.name_len, &err);

    if (Avatar.size == 0)
        return 0;

    if (err != TOX_ERR_FILE_SEND_OK) {
        fprintf(stderr, "tox_file_send failed for friendnumber %d (error %d)\n", friendnum, err);
        return -1;
    }

    struct FileTransfer *ft = new_file_transfer(friendnum, filenum, FILE_TRANSFER_SEND, TOX_FILE_KIND_AVATAR);

    if (!ft)
        return -1;

    ft->file = fopen(Avatar.path, "r");

    if (ft->file == NULL)
        return -1;

    snprintf(ft->file_name, sizeof(ft->file_name), "%s", Avatar.name);
    ft->file_size = Avatar.size;

    return 0;
}

off_t file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
        return 0;

    return st.st_size;
}

int check_file_signature(const char *signature, size_t size, FILE *fp)
{
    char buf[size];
    if (fread(buf, size, 1, fp) != 1)
    {
        return -1;
    }
    int ret = memcmp(signature, buf, size);
    if (fseek(fp, 0L, SEEK_SET) == -1)
    {
        return -1;
    }
    return ret == 0 ? 0 : 1;
}

size_t get_file_name(char *namebuf, size_t bufsize, const char *pathname)
{
    int len = strlen(pathname) - 1;
    char *path = strdup(pathname);

    if (path == NULL)
    {
        // TODO
    }

    while (len >= 0 && pathname[len] == '/')
    {
        path[len--] = '\0';
    }

    char *finalname = strdup(path);

    if (finalname == NULL)
    {
        // TODO
    }

    const char *basenm = strrchr(path, '/');
    if (basenm != NULL)
    {
        if (basenm[1])
        {
            strcpy(finalname, &basenm[1]);
        }
    }

    snprintf(namebuf, bufsize, "%s", finalname);
    free(finalname);
    free(path);

    return strlen(namebuf);
}


int avatar_set(Tox *m, const char *path, size_t path_len)
{
    if (path_len == 0 || path_len >= sizeof(Avatar.path))
    {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return -1;
    }
    char PNG_signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (check_file_signature(PNG_signature, sizeof(PNG_signature), fp) != 0)
    {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    off_t size = file_size(path);

    if (size == 0 || size > MAX_AVATAR_FILE_SIZE)
    {
        return -1;
    }

    get_file_name(Avatar.name, sizeof(Avatar.name), path);
    Avatar.name_len = strlen(Avatar.name);
    snprintf(Avatar.path, sizeof(Avatar.path), "%s", path);
    Avatar.path_len = path_len;
    Avatar.size = size;

    // avatar_send_all(m);

    return 0;
}

static void avatar_clear(void)
{
    memset(&Avatar, 0, sizeof(struct Avatar));
}

void avatar_unset(Tox *m)
{
    avatar_clear();
    // avatar_send_all(m);
}


int main()
{
    Tox *tox = create_tox();

    logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IONBF, 0);

    const char *name = "Echo Bot";
    tox_self_set_name(tox, name, strlen(name), NULL);

    const char *status_message = "Echoing your messages";
    tox_self_set_status_message(tox, status_message, strlen(status_message), NULL);

    bootstrap(tox);

    print_tox_id(tox);

    // init callbacks ----------------------------------
    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_message(tox, friend_message_cb);
    tox_callback_file_chunk_request(tox, on_file_chunk_request);
    tox_callback_self_connection_status(tox, self_connection_status_cb);
    // init callbacks ----------------------------------

    update_savedata_file(tox);

    char path[300];
    snprintf(path, sizeof(path), "%s", "./avatar.png");
    int len = strlen(path) - 1;
    avatar_set(tox, path, len);
    
    while (1)
    {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox) * 1000);
    }

    tox_kill(tox);

    if (logfile)
    {
        fclose(logfile);
        logfile = NULL;
    }

    return 0;
}

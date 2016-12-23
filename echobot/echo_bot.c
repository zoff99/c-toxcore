/*
 *
 * Zoff <zoff@zoff.cc>
 *
 * dirty hack (echobot and toxic were used as blueprint)
 *
 *
 * compile on linux (dynamic):
 *  gcc -O2 -fPIC -o echo_bot echo_bot.c -std=gnu99 -lsodium -I/usr/local/include/ -ltoxcore
 * compile for debugging (dynamic):
 *  gcc -O0 -g -fPIC -o echo_bot echo_bot.c -std=gnu99 -lsodium -I/usr/local/include/ -ltoxcore
 *
 * compile on linux (static):
 *  gcc -O2 -o echo_bot_static echo_bot.c -static -std=gnu99 -L/usr/local/lib -I/usr/local/include/ \
 *   -lsodium -ltoxcore -ltoxgroup -ltoxmessenger -ltoxfriends -ltoxnetcrypto \
 *   -ltoxdht -ltoxnetwork -ltoxcrypto -lsodium -lpthread -static-libgcc -static-libstdc++
 *
 *
 *
 */


#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

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
// #define PATH_MAX 255
#define TIME_STR_SIZE 32
#define MAX_STR_SIZE 200

#define KiB 1024
#define MiB 1048576       /* 1024^2 */
#define GiB 1073741824    /* 1024^3 */

#define seconds_since_last_mod 2 // how long to wait before we process image files in seconds
#define MAX_FILES 1000

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
    char worksubdir[MAX_STR_SIZE];
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


void on_avatar_chunk_request(Tox *m, struct FileTransfer *ft, uint64_t position, size_t length);
int avatar_send(Tox *m, uint32_t friendnum);
struct FileTransfer *new_file_transfer(uint32_t friendnum, uint32_t filenum, FILE_TRANSFER_DIRECTION direction, uint8_t type);

const char *savedata_filename = "savedata.tox";
const char *savedata_tmp_filename = "savedata.tox.tmp";
const char *log_filename = "echobot.log";
const char *my_avatar_filename = "avatar.png";
const char *motion_pics_dir = "./m/";
const char *motion_pics_work_dir = "./work/";
const char *motion_capture_file_extension = ".jpg";
const char *motion_capture_file_extension_mov = ".avi";

FILE *logfile = NULL;
FriendsList Friends;

time_t get_unix_time(void)
{
    return time(NULL);
}


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

        int dummy = fread(savedata, fsize, 1, f);
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

off_t file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
    {
        return 0;
    }

    return st.st_size;
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
        int fd = fileno(logfile);
        fsync(fd);
    }
}


static int find_friend_in_friendlist(uint32_t friendnum)
{
	int i;

	for (i = 0; i <= Friends.max_idx; ++i)
	{
        if (Friends.list[i].num == friendnum)
		{
			return i;
		}
	}

	return -1;
}

static void update_friend_last_online(uint32_t num, time_t timestamp)
{
    Friends.list[num].last_online.last_on = timestamp;
    Friends.list[num].last_online.tm = *localtime((const time_t *)&timestamp);

    /* if the format changes make sure TIME_STR_SIZE is the correct size */
    // const char *t = user_settings->timestamp_format;
    // strftime(Friends.list[num].last_online.hour_min_str, TIME_STR_SIZE, t,
    //         &Friends.list[num].last_online.tm);
}

void send_file_to_friend(Tox *m, uint32_t num, const char* filename)
{
    // ------- hack to send file --------
    // ------- hack to send file --------
    const char *errmsg = NULL;
    char path[MAX_STR_SIZE];
    snprintf(path, sizeof(path), "%s", filename);
    int path_len = strlen(path) - 1;

    FILE *file_to_send = fopen(path, "r");
    if (file_to_send == NULL)
    {
       fprintf(stderr, "error opening file\n");
       return;
    }
    off_t filesize = file_size(path);
    if (filesize == 0)
    {
      fprintf(stderr, "filesize 0\n");
      fclose(file_to_send);
      return;
    }

    char file_name[TOX_MAX_FILENAME_LENGTH];
    size_t namelen = get_file_name(file_name, sizeof(file_name), path);

    TOX_ERR_FILE_SEND err;
    uint32_t filenum = tox_file_send(m, num, TOX_FILE_KIND_DATA, (uint64_t) filesize, NULL,
                                     (uint8_t *) file_name, namelen, &err);

    if (err != TOX_ERR_FILE_SEND_OK)
    {
        printf("! TOX_ERR_FILE_SEND_OK\n");
        goto on_send_error;
    }

    struct FileTransfer *ft = new_file_transfer(num, filenum, FILE_TRANSFER_SEND, TOX_FILE_KIND_DATA);

    if (!ft)
    {
        printf("ft=NULL\n");
        err = TOX_ERR_FILE_SEND_TOO_MANY;
        goto on_send_error;
    }

    memcpy(ft->file_name, file_name, namelen + 1);
    ft->file = file_to_send;
    ft->file_size = filesize;
    tox_file_get_file_id(m, num, filenum, ft->file_id, NULL);

    return;

on_send_error:

    switch (err) {
        case TOX_ERR_FILE_SEND_FRIEND_NOT_FOUND:
            errmsg = "File transfer failed: Invalid friend.";
            break;

        case TOX_ERR_FILE_SEND_FRIEND_NOT_CONNECTED:
            errmsg = "File transfer failed: Friend is offline.";
            break;

        case TOX_ERR_FILE_SEND_NAME_TOO_LONG:
            errmsg = "File transfer failed: Filename is too long.";
            break;

        case TOX_ERR_FILE_SEND_TOO_MANY:
            errmsg = "File transfer failed: Too many concurrent file transfers.";
            break;

        default:
            errmsg = "File transfer failed.";
            break;
    }

    printf("ft error=%s\n", errmsg);
    tox_file_control(m, num, filenum, TOX_FILE_CONTROL_CANCEL, NULL);
    fclose(file_to_send);



    // ------- hack to send file --------
    // ------- hack to send file --------
}


int copy_file(const char *to, const char *from)
{
	int in_fd = open(from, O_RDONLY);
	int out_fd = open(to, O_WRONLY);
	char buf[8192];

	while (1)
	{
	    ssize_t result = read(in_fd, &buf[0], sizeof(buf));
	    if (!result)
	    {
		    break;
	    }
	    write(out_fd, &buf[0], result);
	}
}


char* copy_file_to_friend_subdir(int friendlistnum, const char* file_with_path, const char* filename)
{
	char *newname = NULL;
	newname = malloc(300);
	snprintf(newname, 299, "%s/%s", (const char*)Friends.list[Friends.max_idx].worksubdir, filename);
	copy_file(file_with_path, newname);
	return newname;
}

void send_file_to_all_friends(Tox *m, const char* file_with_path, const char* filename)
{
    size_t i;
    size_t numfriends = tox_self_get_friend_list_size(m);
	char *newname = NULL;
	int j = -1;

    for (i = 0; i < numfriends; ++i)
    {
        printf("sending file (%s) to friendnum=%d\n", file_with_path, (int)i);

		j = find_friend_in_friendlist((uint32_t) i);
		if (j > -1)
		{
			newname = copy_file_to_friend_subdir((int) j, file_with_path, filename);
			send_file_to_friend(m, i, newname);
			free(newname);
			newname = NULL;
		}
		unlink(file_with_path)
    }
}

void friendlist_onConnectionChange(Tox *m, uint32_t num, TOX_CONNECTION connection_status, void *user_data)
{
    printf("friendlist_onConnectionChange:friendnum=%d %d\n", (int)num, (int)connection_status);

    if (avatar_send(m, num) == -1)
    {
        fprintf(stderr, "avatar_send failed for friend %d\n", num);
    }
    Friends.list[num].connection_status = connection_status;
    update_friend_last_online(num, get_unix_time());
    // store_data(m, DATA_FILE);
}



void friendlist_onFriendAdded(Tox *m, uint32_t num, bool sort)
{
    // printf("friendlist_onFriendAdded:001\n");

    if (Friends.max_idx == 0)
    {
        Friends.list = malloc(sizeof(ToxicFriend));
    }
    else
    {
        Friends.list = realloc(Friends.list, ((Friends.max_idx + 1) * sizeof(ToxicFriend)));
    }

    memset(&Friends.list[Friends.max_idx], 0, sizeof(ToxicFriend)); // fill friend with "0" bytes


	printf("friendlist_onFriendAdded:003:%d\n", (int)Friends.max_idx);
	Friends.list[Friends.max_idx].num = num;
	Friends.list[Friends.max_idx].active = true;
	Friends.list[Friends.max_idx].connection_status = TOX_CONNECTION_NONE;
	Friends.list[Friends.max_idx].status = TOX_USER_STATUS_NONE;
	// Friends.list[i].logging_on = (bool) user_settings->autolog == AUTOLOG_ON;

	TOX_ERR_FRIEND_GET_PUBLIC_KEY pkerr;
	tox_friend_get_public_key(m, num, (uint8_t *) Friends.list[Friends.max_idx].pub_key, &pkerr);

	if (pkerr != TOX_ERR_FRIEND_GET_PUBLIC_KEY_OK)
	{
		fprintf(stderr, "tox_friend_get_public_key failed (error %d)\n", pkerr);
	}
	else
	{
		fprintf(stderr, "friend pubkey=%s\n", Friends.list[Friends.max_idx].pub_key);
	}

	// mkdir subdir of workdir for this friend
	snprintf(Friends.list[Friends.max_idx].worksubdir, sizeof(Friends.list[Friends.max_idx].worksubdir), "%s/%s/", motion_pics_work_dir, (const char*)Friends.list[Friends.max_idx].pub_key);
	printf("friend subdir=%s\n", Friends.list[Friends.max_idx].worksubdir);
    mkdir(Friends.list[Friends.max_idx].worksubdir, S_IRWXU | S_IRWXG); // og+rwx

	// TOX_ERR_FRIEND_GET_LAST_ONLINE loerr;
	// time_t t = tox_friend_get_last_online(m, num, &loerr);

	// if (loerr != TOX_ERR_FRIEND_GET_LAST_ONLINE_OK)
	//{
	//    t = 0;
	//}

    Friends.max_idx++;

}



static void load_friendlist(Tox *m)
{
    size_t i;
    size_t numfriends = tox_self_get_friend_list_size(m);

    for (i = 0; i < numfriends; ++i)
    {
        friendlist_onFriendAdded(m, i, false);
        printf("loading friend num:%d pubkey=%s\n", (int)i, Friends.list[Friends.max_idx - 1].pub_key);
    }
}




void close_file_transfer(Tox *m, struct FileTransfer *ft, int CTRL)
{
    printf("close_file_transfer:001\n");

    if (!ft)
        return;

    if (ft->state == FILE_TRANSFER_INACTIVE)
        return;

    if (ft->file)
        fclose(ft->file);

    if (CTRL >= 0)
        tox_file_control(m, ft->friendnum, ft->filenum, (TOX_FILE_CONTROL) CTRL, NULL);

    memset(ft, 0, sizeof(struct FileTransfer));
}



struct FileTransfer *get_file_transfer_struct(uint32_t friendnum, uint32_t filenum)
{
    size_t i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft_send = &Friends.list[friendnum].file_sender[i];

        if (ft_send->state != FILE_TRANSFER_INACTIVE && ft_send->filenum == filenum)
        {
            return ft_send;
        }

        struct FileTransfer *ft_recv = &Friends.list[friendnum].file_receiver[i];

        if (ft_recv->state != FILE_TRANSFER_INACTIVE && ft_recv->filenum == filenum)
        {
            return ft_recv;
        }
    }

    return NULL;
}


void friend_request_cb(Tox *tox, const uint8_t *public_key, const uint8_t *message, size_t length,
                                   void *user_data)
{
    uint32_t friendnum = tox_friend_add_norequest(tox, public_key, NULL);
    printf("add friend:002:friendnum=%d max_id=%d\n", friendnum, (int)Friends.max_idx);
    friendlist_onFriendAdded(tox, friendnum, 0);

    update_savedata_file(tox);
}

void friend_message_cb(Tox *tox, uint32_t friend_number, TOX_MESSAGE_TYPE type, const uint8_t *message,
                                   size_t length, void *user_data)
{
    if (type == TOX_MESSAGE_TYPE_NORMAL)
    {
      printf("message from friend:%s\n", (char*)message);
    }
    else
    {
      printf("message from friend\n");
    }

    tox_friend_send_message(tox, friend_number, type, message, length, NULL);
}



void on_file_recv_chunk(Tox *m, uint32_t friendnumber, uint32_t filenumber, uint64_t position,
                        const uint8_t *data, size_t length, void *user_data)
{
    struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

    if (!ft)
	{
        return;
	}
}


void on_file_recv(Tox *m, uint32_t friendnumber, uint32_t filenumber, uint32_t kind, uint64_t file_size,
                  const uint8_t *filename, size_t filename_length, void *userdata)
{
    /* We don't care about receiving avatars */
    if (kind != TOX_FILE_KIND_DATA)
    {
        tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_CANCEL, NULL);
        printf("on_file_recv:002:cancel incoming avatar\n");
        return;
    }
    else
    {
        // cancel all filetransfers. we don't want to receive files
        tox_file_control(m, friendnumber, filenumber, TOX_FILE_CONTROL_CANCEL, NULL);
        printf("on_file_recv:003:cancel incoming file\n");
        return;
    }
}



void on_file_chunk_request(Tox *tox, uint32_t friendnumber, uint32_t filenumber, uint64_t position,
                           size_t length, void *userdata)
{
    printf("on_file_chunk_request:001:friendnum=%d filenum=%d position=%ld len=%d\n", (int)friendnumber, (int)filenumber, (long)position, (int)length);
    struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

    if (!ft)
    {
        printf("on_file_chunk_request:003 ft=NULL\n");
        return;
    }

    if (ft->file_type == TOX_FILE_KIND_AVATAR)
    {
        on_avatar_chunk_request(tox, ft, position, length);
        return;
    }


    if (ft->state != FILE_TRANSFER_STARTED)
    {
        printf("on_file_chunk_request:005 !FILE_TRANSFER_STARTED\n");
        return;
    }

    if (length == 0)
    {
        printf("File '%s' successfully sent\n", ft->file_name);
        close_file_transfer(tox, ft, -1);
		// also remove the file from disk
		unlink(ft->file_name);
        return;
    }

    if (ft->file == NULL)
    {
        printf("File transfer for '%s' failed: Null file pointer\n", ft->file_name);
        close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    if (ft->position != position)
    {
        if (fseek(ft->file, position, SEEK_SET) == -1)
        {
            printf("File transfer for '%s' failed: Seek fail\n", ft->file_name);
            close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
            return;
        }

        ft->position = position;
    }

    uint8_t send_data[length];
    size_t send_length = fread(send_data, 1, sizeof(send_data), ft->file);

    if (send_length != length)
    {
        printf("File transfer for '%s' failed: Read fail\n", ft->file_name);
        close_file_transfer(tox, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(tox, friendnumber, filenumber, position, send_data, send_length, &err);

    if (err != TOX_ERR_FILE_SEND_CHUNK_OK)
    {
        printf("tox_file_send_chunk failed in chat callback (error %d)\n", err);
    }

    ft->position += send_length;
    ft->bps += send_length;
    ft->last_keep_alive = get_unix_time();

}


void on_avatar_file_control(Tox *m, struct FileTransfer *ft, TOX_FILE_CONTROL control)
{
    switch (control) {
        case TOX_FILE_CONTROL_RESUME:
            if (ft->state == FILE_TRANSFER_PENDING) {
                ft->state = FILE_TRANSFER_STARTED;
            } else if (ft->state == FILE_TRANSFER_PAUSED) {
                ft->state = FILE_TRANSFER_STARTED;
            }

            break;

        case TOX_FILE_CONTROL_PAUSE:
            ft->state = FILE_TRANSFER_PAUSED;
            break;

        case TOX_FILE_CONTROL_CANCEL:
            close_file_transfer(m, ft, -1);
            break;
    }
}


void on_file_control(Tox *m, uint32_t friendnumber, uint32_t filenumber, TOX_FILE_CONTROL control,
                     void *userdata)
{
    struct FileTransfer *ft = get_file_transfer_struct(friendnumber, filenumber);

    if (!ft)
    {
        return;
    }

    if (ft->file_type == TOX_FILE_KIND_AVATAR)
    {
        on_avatar_file_control(m, ft, control);
        return;
    }

    printf("on_file_control:002:file in/out\n");

	char msg[MAX_STR_SIZE];

	switch (control)
	{
		case TOX_FILE_CONTROL_RESUME:
		{
			ft->last_keep_alive = get_unix_time();

			/* transfer is accepted */
			if (ft->state == FILE_TRANSFER_PENDING)
			{
				ft->state = FILE_TRANSFER_STARTED;
			}
			else if (ft->state == FILE_TRANSFER_PAUSED)
			{    /* transfer is resumed */
				ft->state = FILE_TRANSFER_STARTED;
			}

			break;
		}

		case TOX_FILE_CONTROL_PAUSE:
		{
			ft->state = FILE_TRANSFER_PAUSED;
			break;
		}

		case TOX_FILE_CONTROL_CANCEL:
		{
			printf("File transfer for '%s' was aborted\n", ft->file_name);
			close_file_transfer(m, ft, -1);
			break;
		}
	}

}



void on_avatar_chunk_request(Tox *m, struct FileTransfer *ft, uint64_t position, size_t length)
{
    printf("on_avatar_chunk_request:001\n");

    if (ft->state != FILE_TRANSFER_STARTED)
    {
        printf("on_avatar_chunk_request:001a:!FILE_TRANSFER_STARTED\n");
        return;
    }

    if (length == 0)
    {
        close_file_transfer(m, ft, -1);
        return;
    }

    if (ft->file == NULL)
	{
        close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    if (ft->position != position)
	{
        if (fseek(ft->file, position, SEEK_SET) == -1)
		{
            close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
            return;
        }

        ft->position = position;
    }

    uint8_t send_data[length];
    size_t send_length = fread(send_data, 1, sizeof(send_data), ft->file);

    if (send_length != length)
    {
        close_file_transfer(m, ft, TOX_FILE_CONTROL_CANCEL);
        return;
    }

    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(m, ft->friendnum, ft->filenum, position, send_data, send_length, &err);

    if (err != TOX_ERR_FILE_SEND_CHUNK_OK)
    {
        fprintf(stderr, "tox_file_send_chunk failed in avatar callback (error %d)\n", err);
    }

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

    printf("new_file_sender:001 friendnum=%d filenum=%d type=%d\n", (int)friendnum, (int) filenum, (int) type);

    for (i = 0; i < MAX_FILES; ++i)
    {
        struct FileTransfer *ft = &Friends.list[friendnum].file_sender[i];

        printf("new_file_sender:002 i=%d\n", (int)i);

        if (ft->state == FILE_TRANSFER_INACTIVE)
        {
            memset(ft, 0, sizeof(struct FileTransfer));
            ft->index = i;
            ft->friendnum = friendnum;
            ft->filenum = filenum;
            ft->file_type = type;
            ft->last_keep_alive = get_unix_time();
            ft->state = FILE_TRANSFER_PENDING;
            ft->direction = FILE_TRANSFER_SEND;

            printf("new_file_sender:003 i=%d\n", (int)i);

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
    printf("avatar_send:001 friendnum=%d\n", (int)friendnum);
    printf("avatar_send:002 %d %s %d\n", (int)Avatar.size, Avatar.name, (int)Avatar.name_len);

    TOX_ERR_FILE_SEND err;
    uint32_t filenum = tox_file_send(m, friendnum, TOX_FILE_KIND_AVATAR, (size_t) Avatar.size,
                                     NULL, (uint8_t *) Avatar.name, Avatar.name_len, &err);

    if (Avatar.size == 0)
    {
        return 0;
    }

    if (err != TOX_ERR_FILE_SEND_OK)
    {
        fprintf(stderr, "tox_file_send failed for _friendnumber %d (error %d)\n", friendnum, err);
        return -1;
    }

    struct FileTransfer *ft = new_file_transfer(friendnum, filenum, FILE_TRANSFER_SEND, TOX_FILE_KIND_AVATAR);

    if (!ft)
    {
        printf("avatar_send:003:ft=NULL\n");
        return -1;
    }

    ft->file = fopen(Avatar.path, "r");

    if (ft->file == NULL)
    {
        printf("avatar_send:004:ft->file=NULL\n");
        return -1;
    }

    snprintf(ft->file_name, sizeof(ft->file_name), "%s", Avatar.name);
    ft->file_size = Avatar.size;

    return 0;
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


void kill_all_file_transfers_friend(Tox *m, uint32_t friendnum)
{
    size_t i;

    for (i = 0; i < MAX_FILES; ++i)
    {
        close_file_transfer(m, &Friends.list[friendnum].file_sender[i], TOX_FILE_CONTROL_CANCEL);
        close_file_transfer(m, &Friends.list[friendnum].file_receiver[i], TOX_FILE_CONTROL_CANCEL);
    }
}

void kill_all_file_transfers(Tox *m)
{
    size_t i;

    for (i = 0; i < Friends.max_idx; ++i)
    {
        kill_all_file_transfers_friend(m, Friends.list[i].num);
    }
}



int avatar_set(Tox *m, const char *path, size_t path_len)
{
    printf("avatar_set:001\n");

    if (path_len == 0 || path_len >= sizeof(Avatar.path))
    {
        return -1;
    }

    printf("avatar_set:002\n");
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

    printf("avatar_set:003\n");

    off_t size = file_size(path);

    if (size == 0 || size > MAX_AVATAR_FILE_SIZE)
    {
        return -1;
    }

    printf("avatar_set:004\n");

    get_file_name(Avatar.name, sizeof(Avatar.name), path);
    Avatar.name_len = strlen(Avatar.name);
    snprintf(Avatar.path, sizeof(Avatar.path), "%s", path);
    Avatar.path_len = path_len;
    Avatar.size = size;

    printf("avatar_set:099\n");

    return 0;
}

static void avatar_clear(void)
{
    memset(&Avatar, 0, sizeof(struct Avatar));
}

void avatar_unset(Tox *m)
{
    avatar_clear();
}

void check_dir(Tox *m)
{
        DIR *d;
        struct dirent *dir;
        d = opendir(motion_pics_dir);
        if (d)
        {
                while ((dir = readdir(d)) != NULL)
                {
                        if (dir->d_type == DT_REG)
                        {
                                const char *ext = strrchr(dir->d_name,'.');
                                if((!ext) || (ext == dir->d_name))
                                {
                                        // wrong fileextension
                                }
                                else
                                {
                                        if(strcmp(ext, motion_capture_file_extension) == 0)
                                        {
                                                // printf("new image:%s\n", dir->d_name);

                                                // move file to work dir
                                                char oldname[300];
                                                snprintf(oldname, sizeof(oldname), "%s/%s", motion_pics_dir, dir->d_name);


                                                struct stat foo;
                                                time_t mtime;
                                                time_t time_now = time(NULL);

                                                stat(oldname, &foo);
                                                mtime = foo.st_mtime; /* seconds since the epoch */

                                                if ((mtime + seconds_since_last_mod) < time_now)
                                                {
                                                        char newname[300];
                                                        snprintf(newname, sizeof(newname), "%s/%s", motion_pics_work_dir, dir->d_name);

                                                        printf("new image:%s\n", dir->d_name);
                                                        printf("move %s -> %s\n", oldname, newname);
                                                        int renname_err = rename(oldname, newname);
                                                        // printf("res=%d\n", renname_err);

                                                        send_file_to_all_friends(m, newname, dir->d_name);
                                                }
                                                else
                                                {
                                                        // printf("new image:%s (still in use ...)\n", dir->d_name);
                                                }
                                        }
                                        else if(strcmp(ext, motion_capture_file_extension_mov) == 0)
                                        {
                                                // printf("new image:%s\n", dir->d_name);

                                                // move file to work dir
                                                char oldname[300];
                                                snprintf(oldname, sizeof(oldname), "%s/%s", motion_pics_dir, dir->d_name);


                                                struct stat foo;
                                                time_t mtime;
                                                time_t time_now = time(NULL);

                                                stat(oldname, &foo);
                                                mtime = foo.st_mtime; /* seconds since the epoch */

                                                if ((mtime + seconds_since_last_mod) < time_now)
                                                {
                                                        char newname[300];
                                                        snprintf(newname, sizeof(newname), "%s/%s", motion_pics_work_dir, dir->d_name);

                                                        printf("new movie:%s\n", dir->d_name);
                                                        printf("move %s -> %s\n", oldname, newname);
                                                        int renname_err = rename(oldname, newname);
                                                        // printf("res=%d\n", renname_err);

                                                        send_file_to_all_friends(m, newname, dir->d_name);
                                                }
                                                else
                                                {
                                                        // printf("new image:%s (still in use ...)\n", dir->d_name);
                                                }
                                        }
                                }
                        }
                }

                closedir(d);
        }
}


int main()
{
    Tox *tox = create_tox();

    logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IONBF, 0);

    // create motion-capture-dir of not already there
    mkdir(motion_pics_dir, S_IRWXU | S_IRWXG); // og+rwx

    // create workdir of not already there
    mkdir(motion_pics_work_dir, S_IRWXU | S_IRWXG); // og+rwx

    const char *name = "Door";
    tox_self_set_name(tox, name, strlen(name), NULL);

    const char *status_message = "This is your Door";
    tox_self_set_status_message(tox, status_message, strlen(status_message), NULL);

    Friends.max_idx = 0;

    bootstrap(tox);

    print_tox_id(tox);

    // init callbacks ----------------------------------
    tox_callback_friend_request(tox, friend_request_cb);
    tox_callback_friend_message(tox, friend_message_cb);
    tox_callback_file_chunk_request(tox, on_file_chunk_request);
    tox_callback_self_connection_status(tox, self_connection_status_cb);
    tox_callback_friend_connection_status(tox, friendlist_onConnectionChange);
    tox_callback_file_recv_control(tox, on_file_control);
    tox_callback_file_recv(tox, on_file_recv);
    tox_callback_file_recv_chunk(tox, on_file_recv_chunk);
    // init callbacks ----------------------------------

    update_savedata_file(tox);
    load_friendlist(tox);

    char path[300];
    snprintf(path, sizeof(path), "%s", my_avatar_filename);
    int len = strlen(path) - 1;
    avatar_set(tox, path, len);

    while (1)
    {
        tox_iterate(tox, NULL);
        usleep(tox_iteration_interval(tox) * 1000);
        check_dir(tox);
    }

    kill_all_file_transfers(tox);
    tox_kill(tox);

    if (logfile)
    {
        fclose(logfile);
        logfile = NULL;
    }

    return 0;
}

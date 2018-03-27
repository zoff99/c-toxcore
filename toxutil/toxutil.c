/*
 * Copyright © 2018 Zoff
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>

#include "Messenger.h"

#include "logger.h"
#include "network.h"
#include "util.h"
#include "tox.h"
#include "toxutil.h"

#include <assert.h>


typedef struct tox_utils_Node {
    uint8_t key[TOX_PUBLIC_KEY_SIZE];
    uint32_t key2;
    void *data;
    struct tox_utils_Node *next;
} tox_utils_Node;

typedef struct tox_utils_List {
    uint32_t size;
    tox_utils_Node *head;
} tox_utils_List;


tox_utils_List global_friend_capability_list;

typedef struct global_friend_capability_entry {
    bool msgv2_cap;
} global_friend_capability_entry;


tox_utils_List global_msgv2_incoming_ft_list;

typedef struct global_msgv2_incoming_ft_entry {
    uint32_t friend_number;
    uint32_t file_number;
    uint32_t kind;
    uint64_t file_size;
    uint8_t msg_data[TOX_MAX_FILETRANSFER_SIZE_MSGV2];
} global_msgv2_incoming_ft_entry;







// ------------ UTILS ------------

static time_t get_unix_time(void)
{
    return time(NULL);
}

/* Returns 1 if timed out, 0 otherwise */
static int timed_out(time_t timestamp, time_t timeout)
{
    return timestamp + timeout <= get_unix_time();
}

/* compares 2 items of length len (e.g.: Tox Pubkeys)
   Returns 0 if they are the same, 1 if they differ
 */
static int check_file_signature(const uint8_t *pubkey1, const uint8_t *pubkey2, size_t len)
{
    int ret = memcmp(pubkey1, pubkey2, len);
    return ret == 0 ? 0 : 1;
}



static void tox_utils_list_init(tox_utils_List *l)
{
    l->size = 0;
    l->head = NULL;
}

static void tox_utils_list_clear(tox_utils_List *l)
{
    tox_utils_Node *head = l->head;
    tox_utils_Node *next_ = NULL;

    while (head) {
        next_ = head->next;

        if (head->data) {
            free(head->data);
        }

        l->size--;
        l->head = next_;
        free(head);
        head = next_;
    }

    l->size = 0;
    l->head = NULL;
}


static void tox_utils_list_add(tox_utils_List *l, uint8_t *key, uint32_t key2, void *data)
{
    tox_utils_Node *n = calloc(1, sizeof(tox_utils_Node));

    memcpy(n->key, key, TOX_PUBLIC_KEY_SIZE);
    n->key2 = key2;
    n->data = data;

    if (l->head == NULL) {
        n->next = NULL;
    } else {
        n->next = l->head;
    }

    l->head = n;
    l->size++;
}

static tox_utils_Node *tox_utils_list_get(tox_utils_List *l, uint8_t *key, uint32_t key2)
{
    tox_utils_Node *head = l->head;

    while (head) {
        if (head->key2 == key2) {
            if (check_file_signature(head->key, key, TOX_PUBLIC_KEY_SIZE) == 0) {
                return head;
            }
        }

        head = head->next;
    }

    return NULL;
}

static void tox_utils_list_remove(tox_utils_List *l, uint8_t *key, uint32_t key2)
{
    tox_utils_Node *head = l->head;
    tox_utils_Node *prev_ = NULL;
    tox_utils_Node *next_ = NULL;

    while (head) {
        prev_ = head;
        next_ = head->next;

        if (head->key2 == key2) {
            if (check_file_signature(head->key, key, TOX_PUBLIC_KEY_SIZE) == 0) {
                if (prev_) {
                    if (next_) {
                        prev_->next = next_;
                    } else {
                        prev_->next = NULL;
                    }
                }

                if (head->data) {
                    free(head->data);
                }

                free(head);
                l->size--;

                if (l->size == 0) {
                    // list empty
                    // TODO: more to do here?
                    l->head = NULL;
                }

                break;
            }
        }

        head = next_;
    }
}

static void tox_utils_list_remove_2(tox_utils_List *l, uint8_t *key)
{
    tox_utils_Node *head = l->head;
    tox_utils_Node *prev_ = NULL;
    tox_utils_Node *next_ = NULL;

    while (head) {
        prev_ = head;
        next_ = head->next;

        if (check_file_signature(head->key, key, TOX_PUBLIC_KEY_SIZE) == 0) {
            if (prev_) {
                if (next_) {
                    prev_->next = next_;
                } else {
                    prev_->next = NULL;
                }
            }

            if (head->data) {
                free(head->data);
            }

            free(head);
            l->size--;

            if (l->size == 0) {
                // list empty
                // TODO: more to do here?
                l->head = NULL;
            }
        }

        head = next_;
    }
}

// ------------ UTILS ------------













// ----------- FUNCS -----------
static int64_t tox_utils_pubkey_to_friendnum(Tox *tox, const uint8_t *public_key)
{
    TOX_ERR_FRIEND_BY_PUBLIC_KEY error;
    uint32_t fnum = tox_friend_by_public_key(tox, public_key, &error);

    if (error == 0) {
        return (int64_t)fnum;
    } else {
        return -1;
    }
}

static bool tox_utils_friendnum_to_pubkey(Tox *tox, uint8_t *public_key, uint32_t friend_number)
{
    TOX_ERR_FRIEND_GET_PUBLIC_KEY error;
    return tox_friend_get_public_key(tox, friend_number, public_key, &error);
}

static bool tox_utils_get_capabilities(Tox *tox, uint32_t friendnumber)
{
    uint8_t *friend_pubkey = calloc(1, TOX_PUBLIC_KEY_SIZE);

    if (friend_pubkey) {
        bool res = tox_utils_friendnum_to_pubkey(tox, friend_pubkey, friendnumber);

        if (res == true) {
            tox_utils_Node *n = tox_utils_list_get(&global_friend_capability_list, friend_pubkey, 0);

            if (n != NULL) {
                free(friend_pubkey);
                return ((global_friend_capability_entry *)(n->data))->msgv2_cap;
            }
        }

        free(friend_pubkey);
    }

    return false;
}

static void tox_utils_set_capabilities(Tox *tox, uint32_t friendnumber, bool cap)
{
    uint8_t *friend_pubkey = calloc(1, TOX_PUBLIC_KEY_SIZE);

    if (friend_pubkey) {
        bool res = tox_utils_friendnum_to_pubkey(tox, friend_pubkey, friendnumber);

        if (res == true) {
            global_friend_capability_entry *data = calloc(1, sizeof(global_friend_capability_entry));
            data->msgv2_cap = cap;

            tox_utils_Node *n = tox_utils_list_get(&global_friend_capability_list, friend_pubkey, 0);

            if (n == NULL) {
                if (cap == true) {
                    tox_utils_list_add(&global_friend_capability_list, friend_pubkey, 0, data);
                    Messenger *m = (Messenger *)tox;
                    LOGGER_WARNING(m->log, "toxutil:set_capabilities(add:1)");
                }
            } else {
                tox_utils_list_remove(&global_friend_capability_list, friend_pubkey, 0);
                Messenger *m = (Messenger *)tox;
                LOGGER_WARNING(m->log, "toxutil:set_capabilities(rm)");

                if (cap == true) {
                    tox_utils_list_add(&global_friend_capability_list, friend_pubkey, 0, data);
                    Messenger *m = (Messenger *)tox;
                    LOGGER_WARNING(m->log, "toxutil:set_capabilities(add:2)");
                }
            }
        }

        free(friend_pubkey);
    }
}

static void tox_utils_send_capabilities(Tox *tox, uint32_t friendnumber)
{
    uint8_t data[3];
    data[0] = 170; // packet ID
    data[1] = 33;
    data[2] = 44;
    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossless_packet(tox, friendnumber, data, 3, &error);
}

static void tox_utils_receive_capabilities(Tox *tox, uint32_t friendnumber, const uint8_t *data,
        size_t length)
{
    if (length == 3) {
        if ((data[0] == 170) && (data[1] == 33) && (data[2] == 44)) {
            Messenger *m = (Messenger *)tox;
            LOGGER_WARNING(m->log, "toxutil:receive_capabilities fnum=%d data=%d% d %d",
                           (int)friendnumber, (int)data[0], (int)data[1], (int)data[2]);

            // friend has message V2 capability
            tox_utils_set_capabilities(tox, friendnumber, true);
        }
    }
}

// ----------- FUNCS -----------




// --- set callbacks ---
void (*tox_utils_selfconnectionstatus)(struct Tox *tox, unsigned int, void *) = NULL;

void tox_utils_callback_self_connection_status(Tox *tox, tox_self_connection_status_cb *callback)
{
    tox_utils_selfconnectionstatus = (void (*)(Tox * tox,
                                      unsigned int, void *))callback;
}


void (*tox_utils_friend_connectionstatuschange)(struct Tox *tox, uint32_t,
        unsigned int, void *) = NULL;

void tox_utils_callback_friend_connection_status(Tox *tox, tox_friend_connection_status_cb *callback)
{
    tox_utils_friend_connectionstatuschange = (void (*)(Tox * tox, uint32_t,
            unsigned int, void *))callback;
    Messenger *m = (Messenger *)tox;
    LOGGER_WARNING(m->log, "toxutil:set callback");
}


void (*tox_utils_friend_losslesspacket)(struct Tox *tox, uint32_t, const uint8_t *,
                                        size_t, void *) = NULL;

void tox_utils_callback_friend_lossless_packet(Tox *tox, tox_friend_lossless_packet_cb *callback)
{
    tox_utils_friend_losslesspacket = (void (*)(Tox * tox, uint32_t,
                                       const uint8_t *, size_t, void *))callback;
}


void (*tox_utils_filerecvcontrol)(struct Tox *tox, uint32_t, uint32_t,
                                  unsigned int, void *) = NULL;

void tox_utils_callback_file_recv_control(Tox *tox, tox_file_recv_control_cb *callback)
{
    tox_utils_filerecvcontrol = (void (*)(Tox * tox, uint32_t, uint32_t,
                                          unsigned int, void *))callback;
}

void (*tox_utils_filechunkrequest)(struct Tox *tox, uint32_t, uint32_t,
                                   uint64_t, size_t, void *) = NULL;

void tox_utils_callback_file_chunk_request(Tox *tox, tox_file_chunk_request_cb *callback)
{
    tox_utils_filechunkrequest = (void (*)(Tox * tox, uint32_t, uint32_t,
                                           uint64_t, size_t, void *))callback;
}

void (*tox_utils_filerecv)(struct Tox *tox, uint32_t, uint32_t,
                           uint32_t, uint64_t, const uint8_t *, size_t, void *) = NULL;

void tox_utils_callback_file_recv(Tox *tox, tox_file_recv_cb *callback)
{
    tox_utils_filerecv = (void (*)(Tox * tox, int32_t, uint32_t,
                                   uint32_t, uint64_t, const uint8_t *, size_t, void *))callback;
}

void (*tox_utils_filerecvchunk)(struct Tox *tox, uint32_t, uint32_t, uint64_t,
                                const uint8_t *, size_t, void *) = NULL;


void tox_utils_callback_file_recv_chunk(Tox *tox, tox_file_recv_chunk_cb *callback)
{
    tox_utils_filerecvchunk = (void (*)(Tox * tox, uint32_t, uint32_t, uint64_t,
                                        const uint8_t *, size_t, void *))callback;
}

void (*tox_utils_friend_message_v2)(struct Tox *tox, uint32_t, const uint8_t *,
                                    size_t) = NULL;

void tox_utils_callback_friend_message_v2(Tox *tox, tox_util_friend_message_v2_cb *callback)
{
    tox_utils_friend_message_v2 = (void (*)(Tox * tox, uint32_t, const uint8_t *,
                                            size_t))callback;
}

Tox *tox_utils_new(const struct Tox_Options *options, TOX_ERR_NEW *error)
{
    tox_utils_list_init(&global_friend_capability_list);
    tox_utils_list_init(&global_msgv2_incoming_ft_list);
    tox_new(options, error);
}

void tox_utils_kill(Tox *tox)
{
    tox_utils_list_clear(&global_friend_capability_list);
    tox_utils_list_clear(&global_msgv2_incoming_ft_list);
    tox_kill(tox);
}

bool tox_utils_friend_delete(Tox *tox, uint32_t friend_number, TOX_ERR_FRIEND_DELETE *error)
{
    tox_friend_delete(tox, friend_number, error);
}

// --- set callbacks ---



void tox_utils_friend_lossless_packet_cb(Tox *tox, uint32_t friend_number, const uint8_t *data,
        size_t length, void *user_data)
{
    // ------- do messageV2 stuff -------
    tox_utils_receive_capabilities(tox, friend_number, data, length);
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_friend_losslesspacket) {
        tox_utils_friend_losslesspacket(tox, friend_number, data, length, user_data);
    }
    // ------- call the real CB function -------
}


void tox_utils_self_connection_status_cb(Tox *tox,
        TOX_CONNECTION connection_status, void *user_data)
{
    // ------- do messageV2 stuff -------
    if (connection_status == TOX_CONNECTION_NONE) {
        // if we go offline ourselves, remove all FT data
        tox_utils_list_clear(&global_msgv2_incoming_ft_list);
    }
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_selfconnectionstatus) {
        tox_utils_selfconnectionstatus(tox, connection_status, user_data);
        Messenger *m = (Messenger *)tox;
        LOGGER_WARNING(m->log, "toxutil:selfconnectionstatus");
    }
    // ------- call the real CB function -------
}


void tox_utils_friend_connection_status_cb(Tox *tox, uint32_t friendnumber,
        TOX_CONNECTION connection_status, void *user_data)
{
    // ------- do messageV2 stuff -------
    if (connection_status == TOX_CONNECTION_NONE) {
        tox_utils_set_capabilities(tox, friendnumber, false);

        // remove FT data from list
        uint8_t *friend_pubkey = calloc(1, TOX_PUBLIC_KEY_SIZE);

        if (friend_pubkey) {
            bool res = tox_utils_friendnum_to_pubkey(tox, friend_pubkey, friendnumber);

            if (res == true) {
                tox_utils_list_remove_2(&global_msgv2_incoming_ft_list, friend_pubkey);
            }

            free(friend_pubkey);
        }
    } else {
        tox_utils_send_capabilities(tox, friendnumber);
    }
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_friend_connectionstatuschange) {
        tox_utils_friend_connectionstatuschange(tox, friendnumber, connection_status, user_data);
        Messenger *m = (Messenger *)tox;
        LOGGER_WARNING(m->log, "toxutil:friend_connectionstatuschange");
    }
    // ------- call the real CB function -------
}


void tox_utils_file_recv_control_cb(Tox *tox, uint32_t friend_number, uint32_t file_number,
                                    TOX_FILE_CONTROL control, void *user_data)
{
    // ------- do messageV2 stuff -------
    // TODO: ---
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_filerecvcontrol) {
        tox_utils_filerecvcontrol(tox, friend_number, file_number, control, user_data);
        Messenger *m = (Messenger *)tox;
        LOGGER_WARNING(m->log, "toxutil:file_recv_control_cb");
    }
    // ------- call the real CB function -------
}


void tox_utils_file_chunk_request_cb(Tox *tox, uint32_t friend_number, uint32_t file_number,
                                     uint64_t position, size_t length, void *user_data)
{
    // ------- do messageV2 stuff -------
    // TODO: ---
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_filechunkrequest) {
        tox_utils_filechunkrequest(tox, friend_number, file_number, position, length, user_data);
        Messenger *m = (Messenger *)tox;
        LOGGER_WARNING(m->log, "toxutil:file_recv_control_cb");
    }
    // ------- call the real CB function -------
}

void tox_utils_file_recv_cb(Tox *tox, uint32_t friend_number, uint32_t file_number,
                            uint32_t kind, uint64_t file_size,
                            const uint8_t *filename, size_t filename_length, void *user_data)
{
    // ------- do messageV2 stuff -------
    if (kind == TOX_FILE_KIND_MESSAGEV2_SEND) {
        global_msgv2_incoming_ft_entry *data = calloc(1, sizeof(global_msgv2_incoming_ft_entry));

        if (data) {
            data->friend_number = friend_number;
            data->file_number = file_number;
            data->kind = kind;
            data->file_size = file_size;

            uint8_t *friend_pubkey = calloc(1, TOX_PUBLIC_KEY_SIZE);

            if (friend_pubkey) {
                bool res = tox_utils_friendnum_to_pubkey(tox, friend_pubkey, friend_number);

                if (res == true) {
                    tox_utils_list_add(&global_msgv2_incoming_ft_list, friend_pubkey,
                                       file_number, data);
                    Messenger *m = (Messenger *)tox;
                    LOGGER_WARNING(m->log, "toxutil:file_recv_cb:TOX_FILE_KIND_MESSAGEV2_SEND:%d:%d",
                                   (int)friend_number, (int)file_number);
                }

                free(friend_pubkey);
            } else {
                free(data);
            }
        }

        return;
    } else if (kind == TOX_FILE_KIND_MESSAGEV2_ANSWER) {
    } else if (kind == TOX_FILE_KIND_MESSAGEV2_ALTER) {
    } else {
        // ------- do messageV2 stuff -------

        // ------- call the real CB function -------
        if (tox_utils_filerecv) {
            tox_utils_filerecv(tox, friend_number, file_number, kind, file_size,
                               filename, filename_length, user_data);
            Messenger *m = (Messenger *)tox;
            LOGGER_WARNING(m->log, "toxutil:file_recv_cb");
        }
        // ------- call the real CB function -------
    }
}

void tox_utils_file_recv_chunk_cb(Tox *tox, uint32_t friend_number, uint32_t file_number,
                                  uint64_t position, const uint8_t *data, size_t length,
                                  void *user_data)
{
    // ------- do messageV2 stuff -------
    uint8_t *friend_pubkey = calloc(1, TOX_PUBLIC_KEY_SIZE);

    if (friend_pubkey) {
        bool res = tox_utils_friendnum_to_pubkey(tox, friend_pubkey, friend_number);

        if (res == true) {
            tox_utils_Node *n = tox_utils_list_get(&global_msgv2_incoming_ft_list,
                                                   friend_pubkey, file_number);

            if (n != NULL) {
                if (((global_msgv2_incoming_ft_entry *)(n->data))->kind == TOX_FILE_KIND_MESSAGEV2_SEND) {
                    if (length == 0) {
                        // FT finished
                        if (tox_utils_friend_message_v2) {
                            const uint8_t *data_ = ((uint8_t *)((global_msgv2_incoming_ft_entry *)
                                                                (n->data))->msg_data);
                            const uint64_t *size_ = ((uint8_t *)((global_msgv2_incoming_ft_entry *)
                                                                 (n->data))->file_size);
                            tox_utils_friend_message_v2(tox, friend_number, data_, (size_t)size_);
                        }

                        // remove FT data from list
                        tox_utils_list_remove(&global_msgv2_incoming_ft_list,
                                              friend_pubkey, file_number);
                    } else {
                        uint8_t *data_ = ((uint8_t *)((global_msgv2_incoming_ft_entry *)(n->data))->msg_data);
                        memcpy((data_ + position), data, length);
                    }

                    free(friend_pubkey);
                    return;
                }
            }
        }

        free(friend_pubkey);
    }
    // ------- do messageV2 stuff -------

    // ------- call the real CB function -------
    if (tox_utils_filerecvchunk) {
        tox_utils_filerecvchunk(tox, friend_number, file_number,
                                position, data, length, user_data);
        Messenger *m = (Messenger *)tox;
        LOGGER_WARNING(m->log, "toxutil:file_recv_chunk_cb");
    }
    // ------- call the real CB function -------
}






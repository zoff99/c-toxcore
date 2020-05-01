/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013 Tox project.
 */

/*
 * Implementation of the client part of docs/Prevent_Tracking.txt (The part that
 * uses the onion stuff to connect to the friend)
 */
#ifndef C_TOXCORE_TOXCORE_ONION_CLIENT_H
#define C_TOXCORE_TOXCORE_ONION_CLIENT_H

#include "net_crypto.h"
#include "onion_announce.h"
#include "ping_array.h"
#include "group_chats.h"

#define MAX_ONION_CLIENTS 8
#define MAX_ONION_CLIENTS_ANNOUNCE 12 // Number of nodes to announce ourselves to.
#define ONION_NODE_PING_INTERVAL 15
#define ONION_NODE_TIMEOUT ONION_NODE_PING_INTERVAL

/* The interval in seconds at which to tell our friends where we are */
#define ONION_DHTPK_SEND_INTERVAL 30
#define DHT_DHTPK_SEND_INTERVAL 20

#define NUMBER_ONION_PATHS 6

/* The timeout the first time the path is added and
   then for all the next consecutive times */
#define ONION_PATH_FIRST_TIMEOUT 4
#define ONION_PATH_TIMEOUT 10
#define ONION_PATH_MAX_LIFETIME 1200
#define ONION_PATH_MAX_NO_RESPONSE_USES 4

#define MAX_STORED_PINGED_NODES 9
#define MIN_NODE_PING_TIME 10

#define ONION_NODE_MAX_PINGS 3

#define MAX_PATH_NODES 32

#define GC_MAX_DATA_LENGTH GC_PUBLIC_ANNOUNCE_MAX_SIZE

/* If no packets are received within that interval tox will
 * be considered offline.
 */
#define ONION_OFFLINE_TIMEOUT (ONION_NODE_PING_INTERVAL * (ONION_NODE_MAX_PINGS+2))

/* Onion data packet ids. */
#define ONION_DATA_DHTPK CRYPTO_PACKET_DHTPK

typedef struct Onion_Client Onion_Client;

DHT *onion_get_dht(const Onion_Client *onion_c);
Net_Crypto *onion_get_net_crypto(const Onion_Client *onion_c);

/* Add a node to the path_nodes bootstrap array.
 *
 * return -1 on failure
 * return 0 on success
 */
int onion_add_bs_path_node(Onion_Client *onion_c, IP_Port ip_port, const uint8_t *public_key);

/* Put up to max_num nodes in nodes.
 *
 * return the number of nodes.
 */
uint16_t onion_backup_nodes(const Onion_Client *onion_c, Node_format *nodes, uint16_t max_num);

/* Add a friend who we want to connect to.
 *
 * return -1 on failure.
 * return the friend number on success or if the friend was already added.
 */
int onion_friend_num(const Onion_Client *onion_c, const uint8_t *public_key);

/* Add a friend who we want to connect to.
 *
 * return -1 on failure.
 * return the friend number on success.
 */
int onion_addfriend(Onion_Client *onion_c, const uint8_t *public_key);

/* Delete a friend.
 *
 * return -1 on failure.
 * return the deleted friend number on success.
 */
int onion_delfriend(Onion_Client *onion_c, int friend_num);

/* Set if friend is online or not.
 * NOTE: This function is there and should be used so that we don't send useless packets to the friend if he is online.
 *
 * is_online 1 means friend is online.
 * is_online 0 means friend is offline
 *
 * return -1 on failure.
 * return 0 on success.
 */
int onion_set_friend_online(Onion_Client *onion_c, int friend_num, uint8_t is_online);

/* Get the ip of friend friendnum and put it in ip_port
 *
 *  return -1, -- if public_key does NOT refer to a friend
 *  return  0, -- if public_key refers to a friend and we failed to find the friend (yet)
 *  return  1, ip if public_key refers to a friend and we found him
 *
 */
int onion_getfriendip(const Onion_Client *onion_c, int friend_num, IP_Port *ip_port);

typedef int recv_tcp_relay_cb(void *object, uint32_t number, IP_Port ip_port, const uint8_t *public_key);

/* Set the function for this friend that will be callbacked with object and number
 * when that friends gives us one of the TCP relays he is connected to.
 *
 * object and number will be passed as argument to this function.
 *
 * return -1 on failure.
 * return 0 on success.
 */
int recv_tcp_relay_handler(Onion_Client *onion_c, int friend_num,
                           recv_tcp_relay_cb *callback, void *object, uint32_t number);

typedef void onion_dht_pk_cb(void *data, int32_t number, const uint8_t *dht_public_key, void *userdata);

/* Set the function for this friend that will be callbacked with object and number
 * when that friend gives us his DHT temporary public key.
 *
 * object and number will be passed as argument to this function.
 *
 * return -1 on failure.
 * return 0 on success.
 */
int onion_dht_pk_callback(Onion_Client *onion_c, int friend_num, onion_dht_pk_cb *function, void *object,
                          uint32_t number);

/* Set a friends DHT public key.
 *
 * return -1 on failure.
 * return 0 on success.
 */
int onion_set_friend_DHT_pubkey(Onion_Client *onion_c, int friend_num, const uint8_t *dht_key);

/* Copy friends DHT public key into dht_key.
 *
 * return 0 on failure (no key copied).
 * return 1 on success (key copied).
 */
unsigned int onion_getfriend_DHT_pubkey(const Onion_Client *onion_c, int friend_num, uint8_t *dht_key);

#define ONION_DATA_IN_RESPONSE_MIN_SIZE (CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_MAC_SIZE)
#define ONION_CLIENT_MAX_DATA_SIZE (MAX_DATA_REQUEST_SIZE - ONION_DATA_IN_RESPONSE_MIN_SIZE)

/* Send data of length length to friendnum.
 * Maximum length of data is ONION_CLIENT_MAX_DATA_SIZE.
 * This data will be received by the friend using the Onion_Data_Handlers callbacks.
 *
 * Even if this function succeeds, the friend might not receive any data.
 *
 * return the number of packets sent on success
 * return -1 on failure.
 */
int send_onion_data(Onion_Client *onion_c, int friend_num, const uint8_t *data, uint16_t length);

typedef int oniondata_handler_cb(void *object, const uint8_t *source_pubkey, const uint8_t *data,
                                 uint16_t len, void *userdata);

/* Function to call when onion data packet with contents beginning with byte is received. */
void oniondata_registerhandler(Onion_Client *onion_c, uint8_t byte, oniondata_handler_cb *cb, void *object);

void do_onion_client(Onion_Client *onion_c);

Onion_Client *new_onion_client(Mono_Time *mono_time, Net_Crypto *c, GC_Session *gc_session);

void kill_onion_client(Onion_Client *onion_c);


/*  return 0 if we are not connected to the network.
 *  return 1 if we are connected with TCP only.
 *  return 2 if we are also connected with UDP.
 */
unsigned int onion_connection_status(const Onion_Client *onion_c);

typedef struct Onion_Node {
    uint8_t     public_key[CRYPTO_PUBLIC_KEY_SIZE];
    IP_Port     ip_port;
    uint8_t     ping_id[ONION_PING_ID_SIZE];
    uint8_t     data_public_key[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t     is_stored;

    uint64_t    added_time;

    uint64_t    timestamp;

    uint64_t    last_pinged;

    uint8_t     unsuccessful_pings;

    uint32_t    path_used;
} Onion_Node;

typedef struct Onion_Client_Paths {
    Onion_Path paths[NUMBER_ONION_PATHS];
    uint64_t last_path_success[NUMBER_ONION_PATHS];
    uint64_t last_path_used[NUMBER_ONION_PATHS];
    uint64_t path_creation_time[NUMBER_ONION_PATHS];
    /* number of times used without success. */
    unsigned int last_path_used_times[NUMBER_ONION_PATHS];
} Onion_Client_Paths;

typedef struct Last_Pinged {
    uint8_t     public_key[CRYPTO_PUBLIC_KEY_SIZE];
    uint64_t    timestamp;
} Last_Pinged;

typedef struct Onion_Friend {
    uint8_t status; /* 0 if friend is not valid, 1 if friend is valid.*/
    uint8_t is_online; /* Set by the onion_set_friend_status function. */

    uint8_t know_dht_public_key; /* 0 if we don't know the dht public key of the other, 1 if we do. */
    uint8_t dht_public_key[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t real_public_key[CRYPTO_PUBLIC_KEY_SIZE];

    Onion_Node clients_list[MAX_ONION_CLIENTS];
    uint8_t temp_public_key[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t temp_secret_key[CRYPTO_SECRET_KEY_SIZE];

    uint64_t last_dht_pk_onion_sent;
    uint64_t last_dht_pk_dht_sent;

    uint64_t last_noreplay;

    uint64_t last_seen;

    Last_Pinged last_pinged[MAX_STORED_PINGED_NODES];
    uint8_t last_pinged_index;

    recv_tcp_relay_cb *tcp_relay_node_callback;
    void *tcp_relay_node_callback_object;
    uint32_t tcp_relay_node_callback_number;

    onion_dht_pk_cb *dht_pk_callback;
    void *dht_pk_callback_object;
    uint32_t dht_pk_callback_number;

    uint32_t run_count;

    uint8_t gc_data[GC_MAX_DATA_LENGTH];
    uint8_t gc_public_key[ENC_PUBLIC_KEY];
    int16_t gc_data_length;
} Onion_Friend;

typedef struct Onion_Data_Handler {
    oniondata_handler_cb *function;
    void *object;
} Onion_Data_Handler;

struct Onion_Client {
    Mono_Time *mono_time;

    DHT     *dht;
    Net_Crypto *c;
    GC_Session *gc_session;
    Networking_Core *net;
    Onion_Friend    *friends_list;
    uint16_t       num_friends;

    Onion_Node clients_announce_list[MAX_ONION_CLIENTS_ANNOUNCE];
    uint64_t last_announce;

    Onion_Client_Paths onion_paths_self;
    Onion_Client_Paths onion_paths_friends;

    uint8_t secret_symmetric_key[CRYPTO_SYMMETRIC_KEY_SIZE];
    uint64_t last_run;
    uint64_t first_run;

    uint8_t temp_public_key[CRYPTO_PUBLIC_KEY_SIZE];
    uint8_t temp_secret_key[CRYPTO_SECRET_KEY_SIZE];

    Last_Pinged last_pinged[MAX_STORED_PINGED_NODES];

    Node_format path_nodes[MAX_PATH_NODES];
    uint16_t path_nodes_index;

    Node_format path_nodes_bs[MAX_PATH_NODES];
    uint16_t path_nodes_index_bs;

    Ping_Array *announce_ping_array;
    uint8_t last_pinged_index;
    Onion_Data_Handler onion_data_handlers[256];

    uint64_t last_packet_recv;

    unsigned int onion_connected;
    bool udp_connected;
};

#endif

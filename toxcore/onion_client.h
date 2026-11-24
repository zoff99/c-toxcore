/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013 Tox project.
 */

/**
 * Implementation of the client part of docs/Prevent_Tracking.txt (The part that
 * uses the onion stuff to connect to the friend)
 */
#ifndef C_TOXCORE_TOXCORE_ONION_CLIENT_H
#define C_TOXCORE_TOXCORE_ONION_CLIENT_H

#include <stdbool.h>

#include "net_crypto.h"
#include "onion_announce.h"
#include "ping_array.h"

#define MAX_ONION_CLIENTS 8
#define MAX_ONION_CLIENTS_ANNOUNCE 12 // Number of nodes to announce ourselves to.
#define ONION_NODE_PING_INTERVAL 15
#define ONION_NODE_TIMEOUT ONION_NODE_PING_INTERVAL

/** The interval in seconds at which to tell our friends where we are */
#define ONION_DHTPK_SEND_INTERVAL 30
#define DHT_DHTPK_SEND_INTERVAL 20

/** Number of short onion DHTPK retries after the first successful send. */
#define ONION_DHTPK_INITIAL_RETRIES 3

/** Startup/recovery window for aggressive friend onion lookup (seconds). */
#define ONION_FRIEND_STARTUP_WINDOW 30
/** Populate interval while the startup/recovery window is active (seconds). */
#define ONION_FRIEND_STARTUP_POPULATE_INTERVAL 1
/** Max lookup requests per populate while few friend nodes are known. */
#define ONION_FRIEND_STARTUP_POPULATE_FANOUT 8
/** Friend node ping interval during the startup/recovery window (seconds). */
#define ONION_FRIEND_STARTUP_NODE_INTERVAL 1
/** Minimum interval between friend diagnostic DEBUG logs (seconds). */
#define ONION_FRIEND_DIAG_LOG_INTERVAL 5
/** Startup window for aggressive self onion announce (seconds). */
#define ONION_SELF_STARTUP_WINDOW 20
/** Self announce populate interval during startup (seconds). */
#define ONION_SELF_STARTUP_POPULATE_INTERVAL 1
/** Self announce ping interval for not-yet-stored nodes during startup (seconds). */
#define ONION_SELF_STARTUP_NOT_ANNOUNCED_INTERVAL 1

#define NUMBER_ONION_PATHS 6

/**
 * The timeout the first time the path is added and
 * then for all the next consecutive times
 */
#define ONION_PATH_FIRST_TIMEOUT 4
#define ONION_PATH_TIMEOUT 10
#define ONION_PATH_MAX_LIFETIME 1200
#define ONION_PATH_MAX_NO_RESPONSE_USES 4

#define MAX_STORED_PINGED_NODES 9
#define MIN_NODE_PING_TIME 10

#define ONION_NODE_MAX_PINGS 3

#define MAX_PATH_NODES 32

#define GCA_MAX_DATA_LENGTH GCA_PUBLIC_ANNOUNCE_MAX_SIZE

/**
 * If no announce response packets are received within this interval tox will
 * be considered offline. We give time for a node to be pinged often enough
 * that it times out, which leads to the network being thoroughly tested as it
 * is replaced.
 */
#define ONION_OFFLINE_TIMEOUT (ONION_NODE_PING_INTERVAL * (ONION_NODE_MAX_PINGS+2))

/** Onion data packet ids. */
#define ONION_DATA_FRIEND_REQ CRYPTO_PACKET_FRIEND_REQ
#define ONION_DATA_DHTPK CRYPTO_PACKET_DHTPK

typedef struct Onion_Client Onion_Client;

non_null()
DHT *onion_get_dht(const Onion_Client *onion_c);
non_null()
Net_Crypto *onion_get_net_crypto(const Onion_Client *onion_c);

/** @brief Add a node to the path_nodes bootstrap array.
 *
 * If a node with the given public key was already in the bootstrap array, this function has no
 * effect and returns successfully. There is currently no way to update the IP/port for a bootstrap
 * node, so if it changes, the Onion_Client must be recreated.
 *
 * @param onion_c The onion client object.
 * @param ip_port IP/port for the bootstrap node.
 * @param public_key DHT public key for the bootstrap node.
 *
 * @retval false on failure
 * @retval true on success
 */
non_null()
bool onion_add_bs_path_node(Onion_Client *onion_c, const IP_Port *ip_port, const uint8_t *public_key);

/** @brief Put up to max_num nodes in nodes.
 *
 * return the number of nodes.
 */
non_null()
uint16_t onion_backup_nodes(const Onion_Client *onion_c, Node_format *nodes, uint16_t max_num);

/** @brief Get the friend_num of a friend.
 *
 * return -1 on failure.
 * return friend number on success.
 */
non_null()
int onion_friend_num(const Onion_Client *onion_c, const uint8_t *public_key);

/** @brief Add a friend who we want to connect to.
 *
 * return -1 on failure.
 * return the friend number on success or if the friend was already added.
 */
non_null()
int onion_addfriend(Onion_Client *onion_c, const uint8_t *public_key);

/** @brief Delete a friend.
 *
 * return -1 on failure.
 * return the deleted friend number on success.
 */
non_null()
int onion_delfriend(Onion_Client *onion_c, int friend_num);

/** @brief Set if friend is online or not.
 *
 * NOTE: This function is there and should be used so that we don't send
 * useless packets to the friend if they are online.
 *
 * return -1 on failure.
 * return 0 on success.
 */
non_null()
int onion_set_friend_online(Onion_Client *onion_c, int friend_num, bool is_online);

/** @brief Internal diagnostics for a friend's onion lookup / DHTPK state.
 *
 * This is intentionally not part of the public tox.h API/ABI. It is meant for
 * tests, profiling, and DEBUG logging.
 */
typedef struct Onion_Friend_Diagnostics {
    uint16_t friend_num;
    bool is_valid;
    bool is_online;
    bool know_dht_public_key;

    uint16_t num_nodes;      // live nodes in the friend lookup list
    uint16_t num_stored;     // live nodes that store the friend's onion key
    bool dhtpk_ready;        // whether the onion DHTPK quorum is satisfied

    uint64_t last_populated;
    uint32_t populate_interval;
    bool lookup_seed_pending;
    uint64_t lookup_startup_until;

    uint64_t last_dht_pk_onion_sent;
    uint64_t last_dht_pk_dht_sent;
    uint32_t dht_pk_onion_send_count;
    int last_dht_pk_onion_result;
    int last_dht_pk_dht_result;
    uint64_t last_dht_pk_recv;

    uint32_t run_count;
    uint32_t pings;
} Onion_Friend_Diagnostics;

/** @brief Fill diagnostics for friend_num.
 *
 * @retval true on success.
 * @retval false if friend_num is invalid or out is nullptr.
 */
non_null(1) nullable(3)
bool onion_get_friend_diagnostics(const Onion_Client *onion_c, uint16_t friend_num,
                                  Onion_Friend_Diagnostics *out);

/** @brief Get the ip of friend friendnum and put it in ip_port
 *
 * @retval -1 if public_key does NOT refer to a friend
 * @retval  0 if public_key refers to a friend and we failed to find the friend (yet)
 * @retval  1 if public_key refers to a friend and we found them
 */
non_null()
int onion_getfriendip(const Onion_Client *onion_c, int friend_num, IP_Port *ip_port);

typedef int recv_tcp_relay_cb(void *object, uint32_t number, const IP_Port *ip_port, const uint8_t *public_key);

/** @brief Set the function for this friend that will be callbacked with object and number
 * when that friend gives us one of the TCP relays they are connected to.
 *
 * object and number will be passed as argument to this function.
 *
 * return -1 on failure.
 * return 0 on success.
 */
non_null()
int recv_tcp_relay_handler(Onion_Client *onion_c, int friend_num,
                           recv_tcp_relay_cb *callback, void *object, uint32_t number);

typedef void onion_dht_pk_cb(void *data, int32_t number, const uint8_t *dht_public_key, void *userdata);

/** @brief Set the function for this friend that will be callbacked with object and number
 * when that friend gives us their DHT temporary public key.
 *
 * object and number will be passed as argument to this function.
 *
 * return -1 on failure.
 * return 0 on success.
 */
non_null()
int onion_dht_pk_callback(Onion_Client *onion_c, int friend_num, onion_dht_pk_cb *function, void *object,
                          uint32_t number);

/** @brief Set a friend's DHT public key.
 *
 * return -1 on failure.
 * return 0 on success.
 */
non_null()
int onion_set_friend_DHT_pubkey(Onion_Client *onion_c, int friend_num, const uint8_t *dht_key);

/** @brief Copy friends DHT public key into dht_key.
 *
 * return 0 on failure (no key copied).
 * return 1 on success (key copied).
 */
non_null()
unsigned int onion_getfriend_DHT_pubkey(const Onion_Client *onion_c, int friend_num, uint8_t *dht_key);

#define ONION_DATA_IN_RESPONSE_MIN_SIZE (CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_MAC_SIZE)
#define ONION_CLIENT_MAX_DATA_SIZE (MAX_DATA_REQUEST_SIZE - ONION_DATA_IN_RESPONSE_MIN_SIZE)

/** @brief Send data of length length to friendnum.
 * Maximum length of data is ONION_CLIENT_MAX_DATA_SIZE.
 * This data will be received by the friend using the Onion_Data_Handlers callbacks.
 *
 * Even if this function succeeds, the friend might not receive any data.
 *
 * return the number of packets sent on success
 * return -1 on failure.
 */
non_null()
int send_onion_data(Onion_Client *onion_c, int friend_num, const uint8_t *data, uint16_t length);

typedef int oniondata_handler_cb(void *object, const uint8_t *source_pubkey, const uint8_t *data,
                                 uint16_t len, void *userdata);

/** Function to call when onion data packet with contents beginning with byte is received. */
non_null(1) nullable(3, 4)
void oniondata_registerhandler(Onion_Client *onion_c, uint8_t byte, oniondata_handler_cb *cb, void *object);

typedef bool onion_group_announce_cb(Onion_Client *onion_c, uint32_t sendback_num, const uint8_t *data,
                                     size_t data_length, void *user_data);

/** Function to call when the onion gets a group announce response. */
non_null(1) nullable(2, 3)
void onion_group_announce_register(Onion_Client *onion_c, onion_group_announce_cb *func, void *user_data);

non_null()
void do_onion_client(Onion_Client *onion_c);

non_null()
Onion_Client *new_onion_client(const Logger *logger, const Random *rng, const Mono_Time *mono_time, Net_Crypto *c);

nullable(1)
void kill_onion_client(Onion_Client *onion_c);


typedef enum Onion_Connection_Status {
    /** We are not connected to the network. */
    ONION_CONNECTION_STATUS_NONE = 0,
    /** We are connected with TCP only. */
    ONION_CONNECTION_STATUS_TCP = 1,
    /** We are also connected with UDP. */
    ONION_CONNECTION_STATUS_UDP = 2,
} Onion_Connection_Status;

non_null()
Onion_Connection_Status onion_connection_status(const Onion_Client *onion_c, bool lan_only_is_online);

typedef struct Onion_Friend Onion_Friend;

non_null() uint16_t onion_get_friend_count(const Onion_Client *const onion_c);
non_null() Onion_Friend *onion_get_friend(const Onion_Client *const onion_c, uint16_t friend_num);
non_null() const uint8_t *onion_friend_get_gc_public_key(const Onion_Friend *const onion_friend);
non_null() const uint8_t *onion_friend_get_gc_public_key_num(const Onion_Client *const onion_c, uint32_t num);
non_null() void onion_friend_set_gc_public_key(Onion_Friend *const onion_friend, const uint8_t *public_key);
non_null(1) nullable(2)
void onion_friend_set_gc_data(Onion_Friend *const onion_friend, const uint8_t *gc_data, uint16_t gc_data_length);
non_null() bool onion_friend_is_groupchat(const Onion_Friend *const onion_friend);

#endif

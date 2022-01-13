/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "msi.h"

#include "../toxcore/tox.h"
#include "../toxcore/logger.h"
#include "../toxcore/util.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define MSI_MAXMSG_SIZE 256

/**
 * Protocol:
 *
 * `|id [1 byte]| |size [1 byte]| |data [$size bytes]| |...{repeat}| |0 {end byte}|`
 */

typedef enum MSIHeaderID {
    ID_REQUEST = 1,
    ID_ERROR,
    ID_CAPABILITIES,
} MSIHeaderID;


typedef enum MSIRequest {
    REQU_INIT,
    REQU_PUSH,
    REQU_POP,
} MSIRequest;


typedef struct MSIHeaderRequest {
    MSIRequest value;
    bool exists;
} MSIHeaderRequest;

typedef struct MSIHeaderError {
    MSIError value;
    bool exists;
} MSIHeaderError;

typedef struct MSIHeaderCapabilities {
    uint8_t value;
    bool exists;
} MSIHeaderCapabilities;


typedef struct MSIMessage {
    MSIHeaderRequest      request;
    MSIHeaderError        error;
    MSIHeaderCapabilities capabilities;
} MSIMessage;


static void msg_init(MSIMessage *dest, MSIRequest request);
static int msg_parse_in(Tox *tox, MSIMessage *dest, const uint8_t *data, uint16_t length);
static uint8_t *msg_parse_header_out(MSIHeaderID id, uint8_t *dest, const void *value, uint8_t value_len,
                                     uint16_t *length);
static int send_message(Tox *tox, uint32_t friend_number, const MSIMessage *msg);
static int send_error(Tox *tox, uint32_t friend_number, MSIError error);
static MSICall *get_call(MSISession *session, uint32_t friend_number);
static MSICall *new_call(MSISession *session, uint32_t friend_number);
static void handle_init(MSICall *call, const MSIMessage *msg);
static void handle_push(MSICall *call, const MSIMessage *msg);
static void handle_pop(MSICall *call, const MSIMessage *msg);
static void handle_msi_packet(Tox *tox, uint32_t friend_number, const uint8_t *data, size_t length2, void *object);

/**
 * Public functions
 */
void msi_register_callback(MSISession *session, msi_action_cb *callback, MSICallbackID id)
{
    if (!session) {
        return;
    }

    pthread_mutex_lock(session->mutex);
    session->callbacks[id] = callback;
    pthread_mutex_unlock(session->mutex);
}

MSISession *msi_new(Tox *tox)
{
    if (tox == nullptr) {
        return nullptr;
    }

    MSISession *retu = (MSISession *)calloc(sizeof(MSISession), 1);

    if (retu == nullptr) {
        LOGGER_API_ERROR(tox, "Allocation failed! Program might misbehave!");
        return nullptr;
    }

    if (create_recursive_mutex(retu->mutex) != 0) {
        LOGGER_API_ERROR(tox, "Failed to init mutex! Program might misbehave");
        free(retu);
        return nullptr;
    }

    retu->tox = tox;

    // register callback
    tox_callback_friend_lossless_packet_per_pktid(tox, handle_msi_packet, PACKET_ID_MSI);

    LOGGER_API_INFO(tox, "New msi session: %p ", (void *)retu);
    return retu;
}

int msi_kill(Tox *tox, MSISession *session, const Logger *log)
{
    if (session == nullptr) {
        LOGGER_API_ERROR(tox, "Tried to terminate non-existing session");
        return -1;
    }

    // UN-register callback
    tox_callback_friend_lossless_packet_per_pktid(tox, nullptr, PACKET_ID_MSI);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_API_ERROR(tox, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (session->calls) {
        MSIMessage msg;
        msg_init(&msg, REQU_POP);

        MSICall *it = get_call(session, session->calls_head);

        while (it) {
            send_message(session->tox, it->friend_number, &msg);
            MSICall *temp_it = it;
            it = it->next;
            kill_call(temp_it); /* This will eventually free session->calls */
        }
    }

    pthread_mutex_unlock(session->mutex);
    pthread_mutex_destroy(session->mutex);

    LOGGER_API_INFO(tox, "Terminated session: %p", (void *)session);
    free(session);
    return 0;
}

/*
 * return true if friend was offline and the call was canceled
 */
bool check_peer_offline_status(Tox *tox, MSISession *session, uint32_t friend_number)
{
    if (!tox) {
        return false;
    }

    if (!session) {
        return false;
    }

    TOX_ERR_FRIEND_QUERY f_con_query_error;
    TOX_CONNECTION f_conn_status = tox_friend_get_connection_status(tox, friend_number, &f_con_query_error);

    if (f_conn_status == TOX_CONNECTION_NONE) {
        /* Friend is now offline */
        LOGGER_API_DEBUG(tox, "Friend %d is now offline", friend_number);

        pthread_mutex_lock(session->mutex);
        MSICall *call = get_call(session, friend_number);

        if (call == nullptr) {
            pthread_mutex_unlock(session->mutex);
            return true;
        }

        invoke_callback(call, MSI_ON_PEERTIMEOUT); /* Failure is ignored */
        kill_call(call);
        pthread_mutex_unlock(session->mutex);
        return true;
    }

    return false;
}

int msi_invite(MSISession *session, MSICall **call, uint32_t friend_number, uint8_t capabilities)
{
    if (!session) {
        return -1;
    }

    LOGGER_API_INFO(session->tox, "Session: %p Inviting friend: %u", (void *)session, friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_API_ERROR(session->tox, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (get_call(session, friend_number) != nullptr) {
        LOGGER_API_ERROR(session->tox, "Already in a call");
        pthread_mutex_unlock(session->mutex);
        return -1;
    }

    MSICall *temp = new_call(session, friend_number);

    if (temp == nullptr) {
        pthread_mutex_unlock(session->mutex);
        return -1;
    }

    temp->self_capabilities = capabilities;

    MSIMessage msg;
    msg_init(&msg, REQU_INIT);

    msg.capabilities.exists = true;
    msg.capabilities.value = capabilities;

    send_message(temp->session->tox, temp->friend_number, &msg);

    temp->state = MSI_CALL_REQUESTING;

    *call = temp;

    LOGGER_API_INFO(session->tox, "Invite sent");
    pthread_mutex_unlock(session->mutex);
    return 0;
}

int msi_hangup(MSICall *call)
{
    if (!call) {
        return -1;
    }

    if (!call->session) {
        return -1;
    }

    MSISession *session = call->session;

    LOGGER_API_INFO(session->tox, "Session: %p Hanging up call with friend: %u", (void *)call->session,
                     call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        // LOGGER_API_ERROR(session->tox, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state == MSI_CALL_INACTIVE) {
        // LOGGER_API_ERROR(session->tox, "Call is in invalid state!");
        pthread_mutex_unlock(session->mutex);
        return -1;
    }

    MSIMessage msg;
    msg_init(&msg, REQU_POP);

    send_message(session->tox, call->friend_number, &msg);

    kill_call(call);
    pthread_mutex_unlock(session->mutex);
    return 0;
}

int msi_answer(MSICall *call, uint8_t capabilities)
{
    if (!call || !call->session) {
        return -1;
    }

    MSISession *session = call->session;

    LOGGER_API_INFO(session->tox, "Session: %p Answering call from: %u", (void *)call->session,
                     call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_API_ERROR(session->tox, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state != MSI_CALL_REQUESTED) {
        /* Though sending in invalid state will not cause anything weird
         * Its better to not do it like a maniac */
        LOGGER_API_ERROR(session->tox, "Call is in invalid state!");
        pthread_mutex_unlock(session->mutex);
        return -1;
    }

    call->self_capabilities = capabilities;

    MSIMessage msg;
    msg_init(&msg, REQU_PUSH);

    msg.capabilities.exists = true;
    msg.capabilities.value = capabilities;

    send_message(session->tox, call->friend_number, &msg);

    call->state = MSI_CALL_ACTIVE;
    pthread_mutex_unlock(session->mutex);

    return 0;
}

int msi_change_capabilities(MSICall *call, uint8_t capabilities)
{
    if (!call || !call->session) {
        return -1;
    }

    MSISession *session = call->session;

    LOGGER_API_INFO(session->tox, "Session: %p Trying to change capabilities to friend %u", (void *)call->session,
                     call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_API_ERROR(session->tox, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state != MSI_CALL_ACTIVE) {
        LOGGER_API_ERROR(session->tox, "Call is in invalid state!");
        pthread_mutex_unlock(session->mutex);
        return -1;
    }

    call->self_capabilities = capabilities;

    MSIMessage msg;
    msg_init(&msg, REQU_PUSH);

    msg.capabilities.exists = true;
    msg.capabilities.value = capabilities;

    send_message(call->session->tox, call->friend_number, &msg);

    pthread_mutex_unlock(session->mutex);
    return 0;
}


/**
 * Private functions
 */
static void msg_init(MSIMessage *dest, MSIRequest request)
{
    memset(dest, 0, sizeof(*dest));
    dest->request.exists = true;
    dest->request.value = request;
}

static bool check_size(Tox *tox, const uint8_t *bytes, int *constraint, uint8_t size)
{
    *constraint -= 2 + size;

    if (*constraint < 1) {
        LOGGER_API_ERROR(tox, "Read over length!");
        return false;
    }

    if (bytes[1] != size) {
        LOGGER_API_ERROR(tox, "Invalid data size!");
        return false;
    }

    return true;
}

/* Assumes size == 1 */
static bool check_enum_high(Tox *tox, const uint8_t *bytes, uint8_t enum_high)
{
    if (bytes[2] > enum_high) {
        LOGGER_API_ERROR(tox, "Failed enum high limit!");
        return false;
    }

    return true;
}

static int msg_parse_in(Tox *tox, MSIMessage *dest, const uint8_t *data, uint16_t length)
{
    /* Parse raw data received from socket into MSIMessage struct */
    assert(dest);

    if (length == 0 || data[length - 1]) { /* End byte must have value 0 */
        LOGGER_API_ERROR(tox, "Invalid end byte");
        return -1;
    }

    memset(dest, 0, sizeof(*dest));

    const uint8_t *it = data;
    int size_constraint = length;

    while (*it) {/* until end byte is hit */
        switch (*it) {
            case ID_REQUEST: {
                LOGGER_API_INFO(tox, "got:ID_REQUEST");
                if (!check_size(tox, it, &size_constraint, 1) ||
                        !check_enum_high(tox, it, REQU_POP)) {
                    return -1;
                }

                dest->request.value = (MSIRequest)it[2];
                dest->request.exists = true;
                it += 3;
                break;
            }

            case ID_ERROR: {
                LOGGER_API_INFO(tox, "got:ID_ERROR");
                if (!check_size(tox, it, &size_constraint, 1) ||
                        !check_enum_high(tox, it, MSI_E_UNDISCLOSED)) {
                    return -1;
                }

                dest->error.value = (MSIError)it[2];
                dest->error.exists = true;
                it += 3;
                break;
            }

            case ID_CAPABILITIES: {
                LOGGER_API_INFO(tox, "got:ID_CAPABILITIES");
                if (!check_size(tox, it, &size_constraint, 1)) {
                    return -1;
                }

                dest->capabilities.value = it[2];
                dest->capabilities.exists = true;
                it += 3;
                break;
            }

            default: {
                LOGGER_API_ERROR(tox, "Invalid id byte");
                return -1;
            }
        }
    }

    if (dest->request.exists == false) {
        LOGGER_API_ERROR(tox, "Invalid request field!");
        return -1;
    }

    return 0;
}

static uint8_t *msg_parse_header_out(MSIHeaderID id, uint8_t *dest, const void *value, uint8_t value_len,
                                     uint16_t *length)
{
    /* Parse a single header for sending */
    assert(dest);
    assert(value);
    assert(value_len);

    *dest = id;
    ++dest;
    *dest = value_len;
    ++dest;

    memcpy(dest, value, value_len);

    *length += (2 + value_len);

    return dest + value_len; /* Set to next position ready to be written */
}

/* Send an msi packet.
 *
 *  return 1 on success
 *  return 0 on failure
 */
static int m_msi_packet(Tox *tox, int32_t friendnumber, const uint8_t *data, uint16_t length)
{
    // TODO(Zoff): make this better later! -------------------
    /* we need to prepend 1 byte (packet id) to data
     * do this without calloc, memcpy and free in the future
     */
    size_t length_new = length + 1;
    uint8_t *data_new = (uint8_t *)calloc(length_new, sizeof(uint8_t));

    if (!data_new) {
        return 0;
    }

    data_new[0] = PACKET_ID_MSI;

    if (length != 0) {
        memcpy(data_new + 1, data, length);
    }

    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    bool res1 = tox_friend_send_lossless_packet(tox, friendnumber, data_new, length_new, &error);
    LOGGER_API_INFO(tox, "tox_friend_send_lossless_packet:fnum=%d res1=%d error=%d", friendnumber, res1, error);

    free(data_new);

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 1;
    }

    return 0;
}

static int send_message(Tox *tox, uint32_t friend_number, const MSIMessage *msg)
{
    assert(tox);

    /* Parse and send message */

    uint8_t parsed[MSI_MAXMSG_SIZE];

    uint8_t *it = parsed;
    uint16_t size = 0;

    if (msg->request.exists) {
        uint8_t cast = msg->request.value;
        it = msg_parse_header_out(ID_REQUEST, it, &cast,
                                  sizeof(cast), &size);
    } else {
        LOGGER_API_INFO(tox, "Must have request field");
        return -1;
    }

    if (msg->error.exists) {
        uint8_t cast = msg->error.value;
        it = msg_parse_header_out(ID_ERROR, it, &cast,
                                  sizeof(cast), &size);
    }

    if (msg->capabilities.exists) {
        it = msg_parse_header_out(ID_CAPABILITIES, it, &msg->capabilities.value,
                                  sizeof(msg->capabilities.value), &size);
    }

    if (it == parsed) {
        LOGGER_API_WARNING(tox, "Parsing message failed; empty message");
        return -1;
    }

    *it = 0;
    ++size;

    if (m_msi_packet(tox, friend_number, parsed, size)) {
        LOGGER_API_INFO(tox, "Sent message:fnum=%d", friend_number);
        return 0;
    }

    return -1;
}

static int send_error(Tox *tox, uint32_t friend_number, MSIError error)
{
    assert(tox);

    /* Send error message */

    LOGGER_API_INFO(tox, "Sending error: %d to friend: %d", error, friend_number);

    MSIMessage msg;
    msg_init(&msg, REQU_POP);

    msg.error.exists = true;
    msg.error.value = error;

    send_message(tox, friend_number, &msg);
    return 0;
}

int invoke_callback(MSICall *call, MSICallbackID cb)
{
    assert(call);

    if (call->session->callbacks[cb]) {
        LOGGER_API_INFO(call->session->tox, "Invoking callback function: %d", cb);

        if (call->session->callbacks[cb](call->session->av, call) != 0) {
            LOGGER_API_WARNING(call->session->tox,
                               "Callback state handling failed, sending error");
            goto FAILURE;
        }

        return 0;
    }

FAILURE:
    /* If no callback present or error happened while handling,
     * an error message will be sent to friend
     */

    if (call->error == MSI_E_NONE) {
        call->error = MSI_E_HANDLE;
    }

    return -1;
}

static MSICall *get_call(MSISession *session, uint32_t friend_number)
{
    assert(session);

    if (session->calls == nullptr || session->calls_tail < friend_number) {
        return nullptr;
    }

    return session->calls[friend_number];
}

static MSICall *new_call(MSISession *session, uint32_t friend_number)
{
    assert(session);

    LOGGER_API_INFO(session->tox, "new call:fnum=%d", friend_number);

    MSICall *rc = (MSICall *)calloc(1, sizeof(MSICall));

    if (rc == nullptr) {
        return nullptr;
    }

    rc->session = session;
    rc->friend_number = friend_number;

    if (session->calls == nullptr) { /* Creating */
        // TODO: this is totally broken
        session->calls = (MSICall **)calloc(friend_number + 1, sizeof(MSICall *));
        // TODO: this is totally broken


        if (session->calls == nullptr) {
            free(rc);
            return nullptr;
        }

        LOGGER_API_INFO(session->tox, "Creating:fnum=%d", friend_number);

        session->calls_tail = friend_number;
        session->calls_head = friend_number;

        LOGGER_API_INFO(session->tox, "Creating:fnum=%d h=%d t=%d bytes=%d",
                    friend_number,
                    session->calls_head,
                    session->calls_tail,
                    (int)((friend_number + 1) * sizeof(MSICall *)));

    } else if (friend_number > session->calls_tail) { /* Appending */
        // TODO: this is totally broken
        MSICall **tmp = (MSICall **)realloc(session->calls, sizeof(MSICall *) * (friend_number + 1));
        // TODO: this is totally broken

        if (tmp == nullptr) {
            free(rc);
            return nullptr;
        }

        session->calls = tmp;

        /* Set fields in between to null */
        for (uint32_t i = session->calls_tail + 1; i < friend_number; ++i) {
            session->calls[i] = nullptr;
        }

        LOGGER_API_INFO(session->tox, "Appending:fnum=%d", friend_number);

        rc->prev = session->calls[session->calls_tail];
        session->calls[session->calls_tail]->next = rc;

        session->calls_tail = friend_number;

        LOGGER_API_INFO(session->tox, "Appending:fnum=%d h=%d t=%d bytes=%d",
                    friend_number,
                    session->calls_head,
                    session->calls_tail,
                    (int)(sizeof(MSICall *) * (friend_number + 1)));        
        
    } else if (session->calls_head > friend_number) { /* Inserting at front */
        rc->next = session->calls[session->calls_head];

        LOGGER_API_INFO(session->tox, "Inserting at front:fnum=%d", friend_number);

        session->calls[session->calls_head]->prev = rc;
        session->calls_head = friend_number;

        LOGGER_API_INFO(session->tox, "Inserting at front:fnum=%d h=%d t=%d", friend_number, session->calls_head, session->calls_tail);        
    } else { /* right in the middle somewhere */
        // find the previous entry
        MSICall *found_prev_entry = nullptr;
        for (uint32_t i=session->calls_head;i<=session->calls_tail;i++)
        {
            if (session->calls[i])
            {
                if (i < friend_number)
                {
                    found_prev_entry = session->calls[i];
                }
                else
                {
                    break;
                }
            }
        }

        // find the next entry
        MSICall *found_next_entry = nullptr;
        for (uint32_t i=session->calls_head;i<=session->calls_tail;i++)
        {
            if (session->calls[i])
            {
                if (i > friend_number)
                {
                    found_next_entry = session->calls[i];
                    break;
                }
            }
        }

        // set chain-links correctly
        rc->prev = found_prev_entry;
        found_prev_entry->next = rc;
        //
        rc->next = found_next_entry;
        found_next_entry->prev = rc;

    }

    session->calls[friend_number] = rc;
    return rc;
}

void kill_call(MSICall *call)
{
    /* Assume that session mutex is locked */
    if (call == nullptr) {
        return;
    }

    MSISession *session = call->session;

    LOGGER_API_INFO(session->tox, "Killing call: %p", (void *)call);

    MSICall *prev = call->prev;
    MSICall *next = call->next;

    if (prev) {
        prev->next = next;
    } else if (next) {
        session->calls_head = next->friend_number;
    } else {
        goto CLEAR_CONTAINER;
    }

    if (next) {
        next->prev = prev;
    } else if (prev) {
        session->calls_tail = prev->friend_number;
    } else {
        goto CLEAR_CONTAINER;
    }

    session->calls[call->friend_number] = nullptr;
    free(call);
    return;

CLEAR_CONTAINER:
    session->calls_head = 0;
    session->calls_tail = 0;
    free(session->calls);
    free(call);
    session->calls = nullptr;
}


/*
 * INIT -> friend sent a message to us, and wants to initiate a call (e.g. friend is trying to call us)
 */
static void handle_init(MSICall *call, const MSIMessage *msg)
{
    assert(call);
    LOGGER_API_INFO(call->session->tox,
                     "Session: %p Handling 'init' friend: %d", (void *)call->session, call->friend_number);

    if (!msg->capabilities.exists) {
        LOGGER_API_WARNING(call->session->tox, "Session: %p Invalid capabilities on 'init'", (void *)call->session);
        call->error = MSI_E_INVALID_MESSAGE;
        goto FAILURE;
    }

    switch (call->state) {
        case MSI_CALL_INACTIVE: {
            /* Call requested */
            LOGGER_API_INFO(call->session->tox,"MSI_CALL_INACTIVE");
            call->peer_capabilities = msg->capabilities.value;
            call->state = MSI_CALL_REQUESTED;

            if (invoke_callback(call, MSI_ON_INVITE) == -1) {
                goto FAILURE;
            }
        }
        break;

        case MSI_CALL_REQUESTING: {
            /* Call starting */
            LOGGER_API_INFO(call->session->tox,"MSI_CALL_REQUESTING:Friend sent an invite, but we are waiting for an call answer to our call");

            /* if 2 friends call each other at the same time */
#if 1
            call->peer_capabilities = msg->capabilities.value;
            call->state = MSI_CALL_ACTIVE;

            if (invoke_callback(call, MSI_ON_START) == -1) {
                goto FAILURE;
            }

            // send the correct answer to the other friend
            MSIMessage out_msg;
            msg_init(&out_msg, REQU_PUSH);
            out_msg.capabilities.exists = true;
            out_msg.capabilities.value = call->self_capabilities;
            send_message(call->session->tox, call->friend_number, &out_msg);
#endif
        }
        break;

        case MSI_CALL_ACTIVE: {
            /* If peer sent init while the call is already
             * active it's probable that he is trying to
             * re-call us while the call is not terminated
             * on our side. We can assume that in this case
             * we can automatically answer the re-call.
             */

            LOGGER_API_INFO(call->session->tox, "MSI_CALL_ACTIVE:Friend is recalling us");

            MSIMessage out_msg;
            msg_init(&out_msg, REQU_PUSH);

            out_msg.capabilities.exists = true;
            out_msg.capabilities.value = call->self_capabilities;

            send_message(call->session->tox, call->friend_number, &out_msg);

            /* If peer changed capabilities during re-call they will
             * be handled accordingly during the next step
             */
        }
        break;

        case MSI_CALL_REQUESTED: {
            LOGGER_API_WARNING(call->session->tox, "MSI_CALL_REQUESTED:Session: %p Invalid state on 'init'", (void *)call->session);
            call->error = MSI_E_INVALID_STATE;
            goto FAILURE;
        }
    }

    return;
FAILURE:
    send_error(call->session->tox, call->friend_number, call->error);
    kill_call(call);
}

/*
 * PUSH -> friend sent a message to us (friend is sending some info, or answering our request to call him)
 */
static void handle_push(MSICall *call, const MSIMessage *msg)
{
    assert(call);

    LOGGER_API_INFO(call->session->tox, "Session: %p Handling 'push' friend: %d", (void *)call->session,
                     call->friend_number);

    if (!msg->capabilities.exists) {
        LOGGER_API_WARNING(call->session->tox, "Session: %p Invalid capabilities on 'push'", (void *)call->session);
        call->error = MSI_E_INVALID_MESSAGE;
        goto FAILURE;
    }

    switch (call->state) {
        case MSI_CALL_ACTIVE: {
            /* Only act if capabilities changed */
            if (call->peer_capabilities != msg->capabilities.value) {
                LOGGER_API_INFO(call->session->tox, "MSI_CALL_ACTIVE:Friend is changing capabilities to: %u", msg->capabilities.value);

                call->peer_capabilities = msg->capabilities.value;

                if (invoke_callback(call, MSI_ON_CAPABILITIES) == -1) {
                    goto FAILURE;
                }
            }
        }
        break;

        case MSI_CALL_REQUESTING: {
            LOGGER_API_INFO(call->session->tox, "MSI_CALL_REQUESTING:Friend answered our call");

            /* Call started */
            call->peer_capabilities = msg->capabilities.value;
            call->state = MSI_CALL_ACTIVE;

            if (invoke_callback(call, MSI_ON_START) == -1) {
                goto FAILURE;
            }
        }
        break;

        /* Pushes during initialization state are ignored */
        case MSI_CALL_INACTIVE: {
            LOGGER_API_WARNING(call->session->tox, "MSI_CALL_INACTIVE:Ignoring invalid push");
        }
        break;

        case MSI_CALL_REQUESTED: {
            LOGGER_API_WARNING(call->session->tox, "MSI_CALL_REQUESTED:Ignoring invalid push");
        }
        break;
    }

    return;

FAILURE:
    send_error(call->session->tox, call->friend_number, call->error);
    kill_call(call);
}

static void handle_pop(MSICall *call, const MSIMessage *msg)
{
    assert(call);

    LOGGER_API_INFO(call->session->tox, "Session: %p Handling 'pop', friend id: %d", (void *)call->session,
                     call->friend_number);

    /* callback errors are ignored */

    if (msg->error.exists) {
        LOGGER_API_WARNING(call->session->tox, "Friend detected an error: %d", msg->error.value);
        call->error = msg->error.value;
        invoke_callback(call, MSI_ON_ERROR);
    } else {
        switch (call->state) {
            case MSI_CALL_INACTIVE: {
                LOGGER_API_ERROR(call->session->tox, "MSI_CALL_INACTIVE:Handling what should be impossible case");
                abort();
            }

            case MSI_CALL_ACTIVE: {
                /* Hangup */
                LOGGER_API_INFO(call->session->tox, "MSI_CALL_ACTIVE:Friend hung up on us");
                invoke_callback(call, MSI_ON_END);
            }
            break;

            case MSI_CALL_REQUESTING: {
                /* Reject */
                LOGGER_API_INFO(call->session->tox, "MSI_CALL_REQUESTING:Friend rejected our call");
                invoke_callback(call, MSI_ON_END);
            }
            break;

            case MSI_CALL_REQUESTED: {
                /* Cancel */
                LOGGER_API_INFO(call->session->tox, "MSI_CALL_REQUESTED:Friend canceled call invite");
                invoke_callback(call, MSI_ON_END);
            }
            break;
        }
    }

    kill_call(call);
}

/* !!hack!! */
MSISession *tox_av_msi_get(void *av);

static void handle_msi_packet(Tox *tox, uint32_t friend_number, const uint8_t *data, size_t length2, void *object)
{
    if (length2 < 2) {
        // we need more than the ID byte for MSI messages
        return;
    }

    // Zoff: is this correct?
    uint16_t length = (uint16_t)(length2 - 1);

    // Zoff: do not show the first byte, its always "PACKET_ID_MSI"
    const uint8_t *data_strip_id_byte = (const uint8_t *)(data + 1);

    LOGGER_API_INFO(tox, "Got msi message:fnum=%d", friend_number);

    void *toxav = NULL;
    tox_get_av_object(tox, (void **)(&toxav));

    if (!toxav) {
        return;
    }

    MSISession *session = tox_av_msi_get(toxav);

    if (!session) {
        return;
    }

    MSIMessage msg;

    if (msg_parse_in(session->tox, &msg, data_strip_id_byte, length) == -1) {
        LOGGER_API_WARNING(tox, "Error parsing message");
        send_error(tox, friend_number, MSI_E_INVALID_MESSAGE);
        return;
    }

    LOGGER_API_INFO(tox, "Successfully parsed message:msg.request.value=%d", (int)msg.request.value);

    pthread_mutex_lock(session->mutex);
    MSICall *call = get_call(session, friend_number);

    if (call == nullptr) {
        if (msg.request.value != REQU_INIT) {
            // send_error(tox, friend_number, MSI_E_STRAY_MESSAGE); // MSI_E_STRAY_MESSAGE is never handled anywhere, so dont send it
            pthread_mutex_unlock(session->mutex);
            return;
        }

        call = new_call(session, friend_number);

        if (call == nullptr) {
            send_error(tox, friend_number, MSI_E_SYSTEM);
            pthread_mutex_unlock(session->mutex);
            return;
        }
    }

    switch (msg.request.value) {
        case REQU_INIT:
            handle_init(call, &msg);
            break;

        case REQU_PUSH:
            handle_push(call, &msg);
            break;

        case REQU_POP:
            handle_pop(call, &msg); /* always kills the call */
            break;
    }

    pthread_mutex_unlock(session->mutex);
}

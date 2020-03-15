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

/*
 * Zoff: disable logging in ToxAV for now
 */
#include <stdio.h>

#undef LOGGER_DEBUG
#define LOGGER_DEBUG(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_ERROR
#define LOGGER_ERROR(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_WARNING
#define LOGGER_WARNING(log, ...) printf(__VA_ARGS__);printf("\n")
#undef LOGGER_INFO
#define LOGGER_INFO(log, ...) printf(__VA_ARGS__);printf("\n")


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


void msg_init(MSIMessage *dest, MSIRequest request);
int msg_parse_in(const Logger *log, MSIMessage *dest, const uint8_t *data, uint16_t length);
uint8_t *msg_parse_header_out(MSIHeaderID id, uint8_t *dest, const void *value, uint8_t value_len, uint16_t *length);
static int send_message(Tox *tox, uint32_t friend_number, const MSIMessage *msg);
int send_error(Tox *tox, uint32_t friend_number, MSIError error);
static MSICall *get_call(MSISession *session, uint32_t friend_number);
MSICall *new_call(MSISession *session, uint32_t friend_number);
void on_peer_status(const Tox *tox, uint32_t friend_number, uint8_t status, void *data);
void handle_init(MSICall *call, const MSIMessage *msg);
void handle_push(MSICall *call, const MSIMessage *msg);
void handle_pop(MSICall *call, const MSIMessage *msg);
void handle_msi_packet(Tox *tox, uint32_t friend_number, const uint8_t *data, size_t length2, void *object);


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
        LOGGER_ERROR(m->log, "Allocation failed! Program might misbehave!");
        return nullptr;
    }

    if (create_recursive_mutex(retu->mutex) != 0) {
        LOGGER_ERROR(m->log, "Failed to init mutex! Program might misbehave");
        free(retu);
        return nullptr;
    }

    retu->tox = tox;

    // register callback
    tox_callback_friend_lossless_packet_per_pktid(tox, handle_msi_packet, PACKET_ID_MSI);

    LOGGER_DEBUG(m->log, "New msi session: %p ", (void *)retu);
    return retu;
}

int msi_kill(Tox *tox, MSISession *session, const Logger *log)
{
    if (session == nullptr) {
        LOGGER_ERROR(log, "Tried to terminate non-existing session");
        return -1;
    }

    // UN-register callback
    tox_callback_friend_lossless_packet_per_pktid(tox, nullptr, PACKET_ID_MSI);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_ERROR(log, "Failed to acquire lock on msi mutex");
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

    LOGGER_DEBUG(log, "Terminated session: %p", (void *)session);
    free(session);
    return 0;
}

int msi_invite(MSISession *session, MSICall **call, uint32_t friend_number, uint8_t capabilities)
{
    LOGGER_DEBUG(session->messenger->log, "msi_invite:session:%p", (void *)session);

    if (!session) {
        return -1;
    }

    LOGGER_DEBUG(session->messenger->log, "Session: %p Inviting friend: %u", (void *)session, friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_ERROR(session->messenger->log, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (get_call(session, friend_number) != nullptr) {
        LOGGER_ERROR(session->messenger->log, "Already in a call");
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

    LOGGER_DEBUG(session->messenger->log, "Invite sent");
    pthread_mutex_unlock(session->mutex);
    return 0;
}

int msi_hangup(MSICall *call)
{
    if (!call || !call->session) {
        return -1;
    }

    MSISession *session = call->session;

    LOGGER_DEBUG(session->messenger->log, "Session: %p Hanging up call with friend: %u", (void *)call->session,
                 call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_ERROR(session->messenger->log, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state == MSI_CALL_INACTIVE) {
        LOGGER_ERROR(session->messenger->log, "Call is in invalid state!");
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

    LOGGER_DEBUG(session->messenger->log, "Session: %p Answering call from: %u", (void *)call->session,
                 call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_ERROR(session->messenger->log, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state != MSI_CALL_REQUESTED) {
        /* Though sending in invalid state will not cause anything weird
         * Its better to not do it like a maniac */
        LOGGER_ERROR(session->messenger->log, "Call is in invalid state!");
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

    LOGGER_DEBUG(session->messenger->log, "Session: %p Trying to change capabilities to friend %u", (void *)call->session,
                 call->friend_number);

    if (pthread_mutex_trylock(session->mutex) != 0) {
        LOGGER_ERROR(session->messenger->log, "Failed to acquire lock on msi mutex");
        return -1;
    }

    if (call->state != MSI_CALL_ACTIVE) {
        LOGGER_ERROR(session->messenger->log, "Call is in invalid state!");
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

int msg_parse_in(const Logger *log, MSIMessage *dest, const uint8_t *data, uint16_t length)
{
    /* Parse raw data received from socket into MSIMessage struct */

#define CHECK_SIZE(bytes, constraint, size)          \
    do {                                             \
        constraint -= 2 + size;                      \
        if (constraint < 1) {                        \
            LOGGER_ERROR(log, "Read over length!");  \
            return -1;                               \
        }                                            \
        if (bytes[1] != size) {                      \
            LOGGER_ERROR(log, "Invalid data size!"); \
            return -1;                               \
        }                                            \
    } while (0)

    /* Assumes size == 1 */
#define CHECK_ENUM_HIGH(bytes, enum_high)                 \
    do {                                                  \
        if (bytes[2] > enum_high) {                       \
            LOGGER_ERROR(log, "Failed enum high limit!"); \
            return -1;                                    \
        }                                                 \
    } while (0)

    assert(dest);

    if (length == 0 || data[length - 1]) { /* End byte must have value 0 */
        LOGGER_ERROR(log, "Invalid end byte");
        return -1;
    }

    memset(dest, 0, sizeof(*dest));

    const uint8_t *it = data;
    int size_constraint = length;

    while (*it) {/* until end byte is hit */
        switch (*it) {
            case ID_REQUEST:
                CHECK_SIZE(it, size_constraint, 1);
                CHECK_ENUM_HIGH(it, REQU_POP);
                dest->request.value = (MSIRequest)it[2];
                dest->request.exists = true;
                it += 3;
                break;

            case ID_ERROR:
                CHECK_SIZE(it, size_constraint, 1);
                CHECK_ENUM_HIGH(it, MSI_E_UNDISCLOSED);
                dest->error.value = (MSIError)it[2];
                dest->error.exists = true;
                it += 3;
                break;

            case ID_CAPABILITIES:
                CHECK_SIZE(it, size_constraint, 1);
                dest->capabilities.value = it[2];
                dest->capabilities.exists = true;
                it += 3;
                break;

            default:
                LOGGER_ERROR(log, "Invalid id byte");
                return -1;
        }
    }

    if (dest->request.exists == false) {
        LOGGER_ERROR(log, "Invalid request field!");
        return -1;
    }

#undef CHECK_ENUM_HIGH
#undef CHECK_SIZE

    return 0;
}

uint8_t *msg_parse_header_out(MSIHeaderID id, uint8_t *dest, const void *value, uint8_t value_len, uint16_t *length)
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
    size_t length_new = length + 1;
    uint8_t *data_new = calloc(1, length_new);

    if (!data_new) {
        return 0;
    }

    data_new[0] = PACKET_ID_MSI;

    if (length != 0) {
        memcpy(data_new + 1, data, length);
    }

    // TODO(Zoff): make this better later! -------------------

    TOX_ERR_FRIEND_CUSTOM_PACKET error;
    tox_friend_send_lossless_packet(tox, friendnumber, data_new, length_new, &error);

    LOGGER_DEBUG(m->log, "send_message:001:error=%d %p %d %d", error, (void *)tox, friendnumber, (int)length_new);

    // TODO(Zoff): make this better later! -------------------
    free(data_new);
    // TODO(Zoff): make this better later! -------------------

    if (error == TOX_ERR_FRIEND_CUSTOM_PACKET_OK) {
        return 1;
    }

    return 0;
}

static int send_message(Tox *tox, uint32_t friend_number, const MSIMessage *msg)
{
    // TODO(iphydf): Don't rely on toxcore internals.
    Messenger *m;
    m = *(Messenger **)tox;
    assert(m);

    LOGGER_DEBUG(m->log, "send_message:001");

    /* Parse and send message */

    uint8_t parsed [MSI_MAXMSG_SIZE];

    uint8_t *it = parsed;
    uint16_t size = 0;

    if (msg->request.exists) {
        LOGGER_DEBUG(m->log, "send_message:002");
        uint8_t cast = msg->request.value;
        it = msg_parse_header_out(ID_REQUEST, it, &cast,
                                  sizeof(cast), &size);
    } else {
        LOGGER_DEBUG(m->log, "Must have request field");
        return -1;
    }

    LOGGER_DEBUG(m->log, "send_message:004");

    if (msg->error.exists) {
        LOGGER_DEBUG(m->log, "send_message:005");
        uint8_t cast = msg->error.value;
        it = msg_parse_header_out(ID_ERROR, it, &cast,
                                  sizeof(cast), &size);
    }

    LOGGER_DEBUG(m->log, "send_message:006");

    if (msg->capabilities.exists) {
        LOGGER_DEBUG(m->log, "send_message:007");
        it = msg_parse_header_out(ID_CAPABILITIES, it, &msg->capabilities.value,
                                  sizeof(msg->capabilities.value), &size);
    }

    LOGGER_DEBUG(m->log, "send_message:008");

    if (it == parsed) {
        LOGGER_WARNING(m->log, "Parsing message failed; empty message");
        return -1;
    }

    LOGGER_DEBUG(m->log, "send_message:009");

    *it = 0;
    ++size;

    if (m_msi_packet(tox, friend_number, parsed, size)) {
        LOGGER_DEBUG(m->log, "Sent message");
        return 0;
    }

    LOGGER_DEBUG(m->log, "send_message:099");

    return -1;
}

int send_error(Tox *tox, uint32_t friend_number, MSIError error)
{
    // TODO(iphydf): Don't rely on toxcore internals.
    Messenger *m;
    m = *(Messenger **)tox;
    assert(m);

    /* Send error message */

    LOGGER_DEBUG(m->log, "Sending error: %d to friend: %d", error, friend_number);

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
        LOGGER_DEBUG(call->session->messenger->log, "Invoking callback function: %d", cb);

        if (call->session->callbacks[cb](call->session->av, call) != 0) {
            LOGGER_WARNING(call->session->messenger->log,
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

MSICall *new_call(MSISession *session, uint32_t friend_number)
{
    assert(session);

    MSICall *rc = (MSICall *)calloc(sizeof(MSICall), 1);

    if (rc == nullptr) {
        return nullptr;
    }

    rc->session = session;
    rc->friend_number = friend_number;

    if (session->calls == nullptr) { /* Creating */
        session->calls = (MSICall **)calloc(sizeof(MSICall *), friend_number + 1);

        if (session->calls == nullptr) {
            free(rc);
            return nullptr;
        }

        session->calls_tail = friend_number;
        session->calls_head = friend_number;
    } else if (session->calls_tail < friend_number) { /* Appending */
        MSICall **tmp = (MSICall **)realloc(session->calls, sizeof(MSICall *) * (friend_number + 1));

        if (tmp == nullptr) {
            free(rc);
            return nullptr;
        }

        session->calls = tmp;

        /* Set fields in between to null */
        uint32_t i = session->calls_tail + 1;

        for (; i < friend_number; ++i) {
            session->calls[i] = nullptr;
        }

        rc->prev = session->calls[session->calls_tail];
        session->calls[session->calls_tail]->next = rc;

        session->calls_tail = friend_number;
    } else if (session->calls_head > friend_number) { /* Inserting at front */
        rc->next = session->calls[session->calls_head];
        session->calls[session->calls_head]->prev = rc;
        session->calls_head = friend_number;
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

    LOGGER_DEBUG(session->messenger->log, "Killing call: %p", (void *)call);

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

#if 0
void on_peer_status(const Tox *tox, uint32_t friend_number, uint8_t status, void *data)
{
    // TODO(iphydf): Don't rely on toxcore internals.
    Messenger *m;
    m = *(Messenger **)tox;

    MSISession *session = (MSISession *)data;

    switch (status) {
        case 0: { /* Friend is now offline */
            LOGGER_DEBUG(m->log, "Friend %d is now offline", friend_number);

            pthread_mutex_lock(session->mutex);
            MSICall *call = get_call(session, friend_number);

            if (call == nullptr) {
                pthread_mutex_unlock(session->mutex);
                return;
            }

            invoke_callback(call, MSI_ON_PEERTIMEOUT); /* Failure is ignored */
            kill_call(call);
            pthread_mutex_unlock(session->mutex);
        }
        break;

        default:
            break;
    }
}
#endif

void handle_init(MSICall *call, const MSIMessage *msg)
{
    assert(call);
    LOGGER_DEBUG(call->session->messenger->log,
                 "Session: %p Handling 'init' friend: %d", (void *)call->session, call->friend_number);

    if (!msg->capabilities.exists) {
        LOGGER_WARNING(call->session->messenger->log, "Session: %p Invalid capabilities on 'init'", (void *)call->session);
        call->error = MSI_E_INVALID_MESSAGE;
        goto FAILURE;
    }

    switch (call->state) {
        case MSI_CALL_INACTIVE: {
            /* Call requested */
            call->peer_capabilities = msg->capabilities.value;
            call->state = MSI_CALL_REQUESTED;

            if (invoke_callback(call, MSI_ON_INVITE) == -1) {
                goto FAILURE;
            }
        }
        break;

        case MSI_CALL_ACTIVE: {
            /* If peer sent init while the call is already
             * active it's probable that he is trying to
             * re-call us while the call is not terminated
             * on our side. We can assume that in this case
             * we can automatically answer the re-call.
             */

            LOGGER_INFO(call->session->messenger->log, "Friend is recalling us");

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

        case MSI_CALL_REQUESTED: // fall-through
        case MSI_CALL_REQUESTING: {
            LOGGER_WARNING(call->session->messenger->log, "Session: %p Invalid state on 'init'", (void *)call->session);
            call->error = MSI_E_INVALID_STATE;
            goto FAILURE;
        }
    }

    return;
FAILURE:
    send_error(call->session->tox, call->friend_number, call->error);
    kill_call(call);
}

void handle_push(MSICall *call, const MSIMessage *msg)
{
    assert(call);

    LOGGER_DEBUG(call->session->messenger->log, "Session: %p Handling 'push' friend: %d", (void *)call->session,
                 call->friend_number);

    if (!msg->capabilities.exists) {
        LOGGER_WARNING(call->session->messenger->log, "Session: %p Invalid capabilities on 'push'", (void *)call->session);
        call->error = MSI_E_INVALID_MESSAGE;
        goto FAILURE;
    }

    switch (call->state) {
        case MSI_CALL_ACTIVE: {
            /* Only act if capabilities changed */
            if (call->peer_capabilities != msg->capabilities.value) {
                LOGGER_INFO(call->session->messenger->log, "Friend is changing capabilities to: %u", msg->capabilities.value);

                call->peer_capabilities = msg->capabilities.value;

                if (invoke_callback(call, MSI_ON_CAPABILITIES) == -1) {
                    goto FAILURE;
                }
            }
        }
        break;

        case MSI_CALL_REQUESTING: {
            LOGGER_INFO(call->session->messenger->log, "Friend answered our call");

            /* Call started */
            call->peer_capabilities = msg->capabilities.value;
            call->state = MSI_CALL_ACTIVE;

            if (invoke_callback(call, MSI_ON_START) == -1) {
                goto FAILURE;
            }
        }
        break;

        /* Pushes during initialization state are ignored */
        case MSI_CALL_INACTIVE: // fall-through
        case MSI_CALL_REQUESTED: {
            LOGGER_WARNING(call->session->messenger->log, "Ignoring invalid push");
        }
        break;
    }

    return;

FAILURE:
    send_error(call->session->tox, call->friend_number, call->error);
    kill_call(call);
}

void handle_pop(MSICall *call, const MSIMessage *msg)
{
    assert(call);

    LOGGER_DEBUG(call->session->messenger->log, "Session: %p Handling 'pop', friend id: %d", (void *)call->session,
                 call->friend_number);

    /* callback errors are ignored */

    if (msg->error.exists) {
        LOGGER_WARNING(call->session->messenger->log, "Friend detected an error: %d", msg->error.value);
        call->error = msg->error.value;
        invoke_callback(call, MSI_ON_ERROR);
    } else {
        switch (call->state) {
            case MSI_CALL_INACTIVE: {
                LOGGER_ERROR(call->session->messenger->log, "Handling what should be impossible case");
                abort();
            }

            case MSI_CALL_ACTIVE: {
                /* Hangup */
                LOGGER_INFO(call->session->messenger->log, "Friend hung up on us");
                invoke_callback(call, MSI_ON_END);
            }
            break;

            case MSI_CALL_REQUESTING: {
                /* Reject */
                LOGGER_INFO(call->session->messenger->log, "Friend rejected our call");
                invoke_callback(call, MSI_ON_END);
            }
            break;

            case MSI_CALL_REQUESTED: {
                /* Cancel */
                LOGGER_INFO(call->session->messenger->log, "Friend canceled call invite");
                invoke_callback(call, MSI_ON_END);
            }
            break;
        }
    }

    kill_call(call);
}

/* !!hack!! */
MSISession *tox_av_msi_get(void *av);

void handle_msi_packet(Tox *tox, uint32_t friend_number, const uint8_t *data, size_t length2, void *object)
{
    if (length2 < 2) {
        // we need more than the ID byte for MSI messages
        return;
    }

    // Zoff: is this correct?
    uint16_t length = (uint16_t)(length2 - 1);

    // Zoff: do not show the first byte, its always "PACKET_ID_MSI"
    const uint8_t *data_strip_id_byte = (const uint8_t *)(data + 1);


    // TODO(iphydf): Don't rely on toxcore internals.
    Messenger *m;
    m = *(Messenger **)tox;

    LOGGER_DEBUG(m->log, "Got msi message");

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

    if (msg_parse_in(m->log, &msg, data_strip_id_byte, length) == -1) {
        LOGGER_WARNING(m->log, "Error parsing message");
        send_error(tox, friend_number, MSI_E_INVALID_MESSAGE);
        return;
    }

    LOGGER_DEBUG(m->log, "Successfully parsed message");

    pthread_mutex_lock(session->mutex);
    MSICall *call = get_call(session, friend_number);

    if (call == nullptr) {
        if (msg.request.value != REQU_INIT) {
            send_error(tox, friend_number, MSI_E_STRAY_MESSAGE);
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

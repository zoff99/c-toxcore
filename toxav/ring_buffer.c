/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013 Tox project.
 * Copyright © 2013 plutooo
 */
#include "ring_buffer.h"

#include <stdlib.h>

struct RingBuffer {
    uint16_t  size; /* Max size */
    uint16_t  start;
    uint16_t  end;
    void    **data;
    uint8_t  *type;
};

bool rb_full(const RingBuffer *b)
{
    return (b->end + 1) % b->size == b->start;
}

bool rb_empty(const RingBuffer *b)
{
    return b->end == b->start;
}

/*
 * returns: NULL on success
            start address of ?? on FAILURE
 */
void *rb_write(RingBuffer *b, void *p, uint8_t data_type_)
{
    void *rc = NULL;

    if ((b->end + 1) % b->size == b->start) { /* full */
        rc = b->data[b->start];
    }

    b->data[b->end] = p;
    b->type[b->end] = data_type_;
    b->end = (b->end + 1) % b->size;

    if (b->end == b->start) {
        b->start = (b->start + 1) % b->size;
    }

    return rc;
}

bool rb_read(RingBuffer *b, void **p, uint8_t *data_type_)
{
    if (b->end == b->start) { /* Empty */
        *p = NULL;
        return false;
    }

    *p = b->data[b->start];
    *data_type_ = b->type[b->start];

    b->start = (b->start + 1) % b->size;
    return true;
}

RingBuffer *rb_new(int size)
{
    RingBuffer *buf = (RingBuffer *)calloc(sizeof(RingBuffer), 1);

    if (!buf) {
        return NULL;
    }

    buf->size = size + 1; /* include empty elem */
    buf->data = (void **)calloc(buf->size, sizeof(void *));

    if (!buf->data) {
        free(buf);
        return NULL;
    }

    if (!(buf->type = (uint8_t *)calloc(buf->size, sizeof(uint8_t)))) {
        // TODO: ???
    }

    return buf;
}

void rb_kill(RingBuffer *b)
{
    if (b) {
        free(b->data);
        free(b->type);
        free(b);
    }
}

uint16_t rb_size(const RingBuffer *b)
{
    if (rb_empty(b)) {
        return 0;
    }

    return
        b->end > b->start ?
        b->end - b->start :
        (b->size - b->start) + b->end;
}

uint16_t rb_data(const RingBuffer *b, void **dest)
{
    uint16_t i = 0;

    for (; i < rb_size(b); ++i) {
        dest[i] = b->data[(b->start + i) % b->size];
    }

    return i;
}




/*
 * Copyright © 2018 zoff@zoff.cc
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

/*
 * TimeStamp Buffer (TSBuffer) — Implementation & Usage Guide
 * ==========================================================
 *
 * OVERVIEW
 * --------
 * TSBuffer is a fixed-capacity circular (ring) buffer where each entry carries
 * a timestamp, an opaque type tag, and a void* data pointer.  It was designed
 * as a jitter buffer for real-time media streams (video frames in ToxAV):
 * frames arrive out of order or with varying network delay, and the consumer
 * asks "give me the oldest frame whose timestamp is close to X".
 *
 * The buffer does NOT sort entries by timestamp.  Entries are stored in
 * insertion (ring) order.  Timestamp-based selection happens only at read time
 * by scanning all live entries.
 *
 *
 * CAPACITY
 * --------
 *   TSBuffer *b = tsb_new(N);
 *
 * Internally allocates N+1 slots.  One slot is always kept empty so the
 * classic ring-buffer trick (start == end → empty) works.  Therefore the
 * usable capacity is exactly N entries.
 *
 * Because all indices are uint16_t, the maximum safe value for N is
 * (UINT16_MAX - 2) = 65533.  Passing a larger value causes silent wrap-around
 * and corruption.
 *
 *
 * MEMORY OWNERSHIP  —  THE CRITICAL PART
 * --------------------------------------
 * TSBuffer takes OWNERSHIP of every pointer you hand to tsb_write().
 * After a successful tsb_write() you must NOT free that pointer yourself.
 * The buffer (or a later caller) will free it.
 *
 *   Allocating:  ALWAYS heap-allocate (malloc / calloc) the data you pass in.
 *                Never pass a stack pointer - the buffer will call free() on it
 *                and you will get an ASAN "bad-free" / glibc "invalid pointer"
 *                abort.
 *
 *   Freeing happens in exactly four places:
 *
 *   1. tsb_write() return value (EVICTED entry)
 *      If the buffer is full when you write, the entry at the ring's `start`
 *      position is evicted and its pointer is RETURNED to you.
 *      You MUST free() it:
 *
 *          void *evicted = tsb_write(b, my_data, type, ts);
 *          if (evicted) free(evicted);   // <-- mandatory
 *
 *      If the buffer was not full, NULL is returned and nothing to free.
 *
 *   2. tsb_read() output parameter (EXTRACTED entry)
 *      On success the matched entry's pointer is stored in *p.
 *      Ownership transfers to you.  You MUST free(*p) after use:
 *
 *          void *frame; uint64_t type; uint32_t ts_out;
 *          uint16_t removed, skip;
 *          if (tsb_read(b, &frame, &type, &ts_out, want_ts, range,
 *                       &removed, &skip)) {
 *              process(frame);
 *              free(frame);              // <-- mandatory
 *          }
 *
 *   3. Internal deletion (tsb_delete_old_entries, called by tsb_read)
 *      After a successful read, tsb_read() silently deletes every entry whose
 *      timestamp is strictly less than (timestamp_in - timestamp_range).
 *      Those entries are freed INTERNALLY - you never see them and must not
 *      try to free them.
 *
 *   4. tsb_drain() / tsb_kill()
 *      tsb_drain() extracts and free()s every remaining entry.
 *      tsb_kill() calls tsb_drain() and then frees the buffer struct itself.
 *      After tsb_kill() the pointer is invalid - do not touch it.
 *
 *   Summary table:
 *   ┌─────────────────────────┬────────────────────────────────────────────┐
 *   │ Event                   │ Who frees the data pointer?                │
 *   ├─────────────────────────┼────────────────────────────────────────────┤
 *   │ tsb_write (not full)    │ Buffer owns it; freed later by read/drain  │
 *   │ tsb_write (full)        │ Caller MUST free the returned pointer      │
 *   │ tsb_read (match found)  │ Caller MUST free *p                        │
 *   │ tsb_read (internal del) │ Buffer frees internally — caller never sees│
 *   │ tsb_drain / tsb_kill    │ Buffer frees everything remaining          │
 *   └─────────────────────────┴────────────────────────────────────────────┘
 *
 *
 * WHAT GETS EVICTED ON A FULL BUFFER?
 * -----------------------------------
 * When the buffer is full and tsb_write() is called, the entry at the
 * physical `start` index of the ring is evicted.  This is the OLDEST entry
 * in INSERTION ORDER, which is NOT necessarily the entry with the smallest
 * timestamp.  (There is a TODO in the source acknowledging this.)
 *
 * Example:
 *     write(A, ts=500)   →  ring: [A]
 *     write(B, ts=100)   →  ring: [A, B]
 *     write(C, ts=300)   →  ring: [A, B, C]   ← buffer now full (capacity 3)
 *     evicted = write(D, ts=400)
 *     → evicted == A  (ts=500), NOT B (ts=100)
 *
 * If you need true oldest-timestamp eviction, you must handle it in the
 * caller before writing.
 *
 *
 * HOW DOES tsb_read() SELECT AN ENTRY?
 * ------------------------------------
 * tsb_read(b, &p, &type, &ts_out, timestamp_in, timestamp_range, ...)
 *
 * 1. It scans ALL live entries and collects those whose timestamp falls in
 *    the inclusive window:
 *
 *        [ timestamp_in - timestamp_range ,  timestamp_in + 1 ]
 *
 *    Note the asymmetric bounds: the lower end is inclusive, and the upper
 *    end is timestamp_in + 1 (also inclusive), so an entry at exactly
 *    timestamp_in + 1 WILL match.
 *
 * 2. Among all matching entries, it picks the one with the SMALLEST
 *    timestamp (the "oldest" in time).  Ties are broken by ring position
 *    (first found wins).
 *
 * 3. The chosen entry is swapped into the `start` slot and extracted.
 *
 * 4. Then tsb_delete_old_entries() is called with
 *    threshold = timestamp_in - timestamp_range.  Every remaining entry
 *    with timestamp STRICTLY LESS than this threshold is deleted and freed
 *    internally.
 *
 * What if there is no exact timestamp match?
 *    → The buffer returns the closest EARLIER entry that still falls inside
 *      the range window.  It will NEVER return an entry with a timestamp
 *      greater than timestamp_in + 1.
 *    → If the range window contains no entries at all, tsb_read() returns
 *      false and *p is set to NULL.  Nothing is deleted.
 *
 * Example (range = 100):
 *     entries: ts=10, ts=150, ts=250, ts=400
 *     tsb_read(want=300, range=100)
 *       window = [200 .. 301]
 *       matches: ts=250  →  returned
 *       internal delete: entries with ts < 200  →  ts=10 and ts=150 freed
 *
 *
 * THE is_skipping OUTPUT
 * ----------------------
 * If the caller's requested window starts AFTER the last timestamp that was
 * read (last_timestamp_out), it means some time range was never consumed.
 * is_skipping is set to the size of that gap:
 *
 *     is_skipping = (timestamp_in - timestamp_range) - last_timestamp_out
 *
 * A non-zero value warns the caller that frames were missed.  This is
 * informational only; the buffer does not act on it.
 *
 *
 * THE removed_entries_back OUTPUT
 * -------------------------------
 * Despite the name, this does NOT report the total number of deleted entries.
 * It counts only those deleted entries whose timestamp was ALSO less than
 * last_timestamp_out (i.e. entries that were already "behind" the last
 * consumed timestamp).  In practice this is almost always 0 because
 * last_timestamp_out starts at 0 and timestamps are typically positive.
 * Do not rely on this value as a general "how many were cleaned up" counter.
 *
 *
 * THE type FIELD
 * --------------
 * The uint64_t `type` stored alongside each entry is entirely opaque to the
 * buffer.  The caller can use it for flags, frame type (keyframe / delta),
 * codec ID, or anything else.  It is returned unchanged by tsb_read().
 *
 *
 * THREAD SAFETY
 * -------------
 * TSBuffer is NOT thread-safe.  All functions operate without locking.
 * If multiple threads produce/consume entries, the caller must provide
 * external synchronisation (e.g. pthread_mutex).  In ToxAV, vc->queue_mutex
 * protects the TSBuffer used for incoming video frames.
 *
 *
 * TYPICAL LIFECYCLE (producer/consumer pattern)
 * ---------------------------------------------
 *     // --- setup ---
 *     TSBuffer *buf = tsb_new(64);           // 64-entry jitter buffer
 *
 *     // --- producer (network receive thread) ---
 *     void *frame = decode_packet(pkt);      // heap-allocated!
 *     void *evicted = tsb_write(buf, frame, flags, record_ts);
 *     if (evicted) {
 *         free(evicted);                     // buffer was full, drop oldest
 *     }
 *
 *     // --- consumer (playback / iterate thread) ---
 *     void *out; uint64_t type; uint32_t ts_out;
 *     uint16_t removed, skip;
 *     uint32_t now = current_playback_time();
 *     if (tsb_read(buf, &out, &type, &ts_out, now, 90, &removed, &skip)) {
 *         render_frame(out);
 *         free(out);                         // caller owns extracted data
 *     }
 *
 *     // --- teardown ---
 *     tsb_kill(buf);                         // frees all remaining entries
 *     buf = NULL;
 *
 *
 * EDGE CASES & CAVEATS
 * --------------------
 * • tsb_new(0) creates a buffer of capacity 0.  It is immediately "full".
 *   Every tsb_write() will evict and return the just-written pointer.
 *   This is technically safe but useless.
 *
 * • Timestamps wrap at UINT32_MAX.  The range comparison in tsb_read()
 *   casts to int64_t, so a small window near 0 or UINT32_MAX works
 *   correctly.  However, a range larger than ~2^31 will produce negative
 *   lower bounds that may match unexpectedly.
 *
 * • tsb_read() with range = UINT32_MAX (as used by tsb_drain) matches
 *   every entry regardless of timestamp.
 *
 * • The internal deletion pass (tsb_delete_old_entries) only runs after a
 *   SUCCESSFUL read.  If tsb_read() finds no matching entry, old entries
 *   are NOT cleaned up, even if they are far outside the window.
 *
 * • Calling tsb_read() on an empty buffer is safe: it returns false and
 *   sets *p = NULL, *removed_entries_back = 0.
 *
 * • Calling tsb_kill(NULL) or tsb_drain(NULL) is safe (no-op).
 */

#include "ts_buffer.h"

#include <stdlib.h>
#include <stdio.h>

struct TSBuffer {
    uint16_t  size; /* max. number of elements in buffer [ MAX ALLOWED = (UINT16MAX - 1) !! ] */
    uint16_t  start;
    uint16_t  end;
    uint64_t  *type; /* used by caller anyway the caller wants, or dont use it at all */
    uint32_t  *timestamp; /* these dont need to be unix timestamp, they can be numbers of a counter */
    uint32_t  last_timestamp_out; /* timestamp of the last read entry */
    void    **data;
};

bool tsb_full(const TSBuffer *b)
{
    return (b->end + 1) % b->size == b->start;
}

bool tsb_empty(const TSBuffer *b)
{
    return b->end == b->start;
}

/*
 * returns: NULL on success
 *          oldest element on FAILURE -> caller must free it after tsb_write() call
 */
void *tsb_write(TSBuffer *b, void *p, const uint64_t data_type, const uint32_t timestamp)
{
    void *rc = NULL;

    if (tsb_full(b) == true) {
        rc = b->data[b->start]; // return oldest element -> TODO: this is not actually the oldest
        // element. --> search for the element with the oldest timestamp and return that!
        b->start = (b->start + 1) % b->size; // include empty element if buffer would be empty now
    }

    b->data[b->end] = p;
    b->type[b->end] = data_type;
    b->timestamp[b->end] = timestamp;
    b->end = (b->end + 1) % b->size;

    // printf("tsb_write:%p size=%d start=%d end=%d\n", (void *)b, b->size, b->start, b->end);
    // tsb_debug_print_entries(b);

    return rc;
}

static void tsb_move_delete_entry(TSBuffer *b, uint16_t src_index, uint16_t dst_index)
{
    free(b->data[dst_index]);

    b->data[dst_index] = b->data[src_index];
    b->type[dst_index] = b->type[src_index];
    b->timestamp[dst_index] = b->timestamp[src_index];

    // just to be safe ---
    b->data[src_index] = NULL;
    b->type[src_index] = 0;
    b->timestamp[src_index] = 0;
    // just to be safe ---
}

static void tsb_close_hole(TSBuffer *b, uint16_t start_index, uint16_t hole_index)
{
    int32_t current_index = (int32_t)hole_index;

    while (true) {
        // delete current index by moving the previous entry into it
        // don't change start element pointer in this function!
        if (current_index < 1) {
            tsb_move_delete_entry(b, (b->size - 1), current_index);
        } else {
            tsb_move_delete_entry(b, (uint16_t)(current_index - 1), current_index);
        }

        if (current_index == (int32_t)start_index) {
            return;
        }

        current_index = current_index - 1;

        if (current_index < 0) {
            current_index = (int32_t)(b->size - 1);
        }
    }
}

static uint16_t tsb_delete_old_entries(TSBuffer *b, const uint64_t timestamp_threshold)
{
    // buffer empty, nothing to delete
    if (tsb_empty(b) == true) {
        return 0;
    }

    uint16_t removed_entries = 0;
    uint16_t removed_entries_before_last_out =
        0; /* entries removed discarding those between threshold and last read entry */
    uint16_t start_entry = b->start;
    uint16_t current_element;
    // iterate all entries

    for (int i = 0; i < tsb_size(b); i++) {
        current_element = (start_entry + i) % b->size;

        if ((uint64_t)b->timestamp[current_element] < (uint64_t)timestamp_threshold) {
            tsb_close_hole(b, start_entry, current_element);

            if ((uint64_t)b->timestamp[current_element] < (uint64_t)b->last_timestamp_out) {
                removed_entries_before_last_out++;
            }

            removed_entries++;
        }
    }

    b->start = (b->start + removed_entries) % b->size;

    return removed_entries_before_last_out;
}

void tsb_get_range_in_buffer(Tox *tox, TSBuffer *b, uint32_t *timestamp_min, uint32_t *timestamp_max)
{
    uint16_t current_element;
    uint16_t start_entry = b->start;
    *timestamp_min = UINT32_MAX;
    *timestamp_max = 0;

    for (int i = 0; i < tsb_size(b); i++) {
        current_element = (start_entry + i) % b->size;

        if ((uint64_t)b->timestamp[current_element] >= (uint64_t)*timestamp_max) {
            *timestamp_max = b->timestamp[current_element];
        }

        if ((uint64_t)b->timestamp[current_element] <= (uint64_t)*timestamp_min) {
            *timestamp_min = b->timestamp[current_element];
        }
    }
}

static bool tsb_return_oldest_entry_in_range(TSBuffer *b, void **p, uint64_t *data_type,
        uint32_t *timestamp_out,
        const uint32_t timestamp_in, const uint32_t timestamp_range)
{
    int32_t found_element = -1;
    uint32_t found_timestamp = UINT32_MAX;
    uint16_t start_entry = b->start;
    uint16_t current_element;

    for (int i = 0; i < tsb_size(b); i++) {
        current_element = (start_entry + i) % b->size;

        if ((((int64_t)b->timestamp[current_element]) >= ((int64_t)timestamp_in - (int64_t)timestamp_range))
                &&
                ((int64_t)b->timestamp[current_element] <= ((int64_t)timestamp_in + (int64_t)1))) {
            // printf("tsb_return_oldest_entry_in_range:1:%p data=%p\n", (void *)b, (void *)b->data[current_element]);
            // timestamp of entry is in range
            if ((int64_t)b->timestamp[current_element] < (int64_t)found_timestamp) {
                // printf("tsb_return_oldest_entry_in_range:2:%p data=%p\n", (void *)b, (void *)b->data[current_element]);
                // entry is older than previous found entry, or is the first found entry
                found_timestamp = (uint32_t)b->timestamp[current_element];
                found_element = (int32_t)current_element;
            }
        }
    }

    if (found_element > -1) {

        // printf("tsb_return_oldest_entry_in_range:%p found_element=%u\n", (void *)b, found_element);

        // swap element with element in "start" position
        if (found_element != (int32_t)b->start) {
            void *p_save = b->data[found_element];
            uint64_t data_type_save = b->type[found_element];
            uint32_t timestamp_save = b->timestamp[found_element];

            b->data[found_element] = b->data[b->start];
            b->type[found_element] = b->type[b->start];
            b->timestamp[found_element] = b->timestamp[b->start];

            b->data[b->start] = p_save;
            b->type[b->start] = data_type_save;
            b->timestamp[b->start] = timestamp_save;
        }

        // fill data to return to caller
        *p = b->data[b->start];
        *data_type = b->type[b->start];
        *timestamp_out = b->timestamp[b->start];

        b->data[b->start] = NULL;
        b->timestamp[b->start] = 0;
        b->type[b->start] = 0;

        // change start element pointer
        b->start = (b->start + 1) % b->size;
        return true;
    }

    *p = NULL;
    return false;
}

#if 0
static bool tsb_return_newest_entry_in_range(TSBuffer *b, void **p, uint64_t *data_type,
        uint32_t *timestamp_out,
        const uint32_t timestamp_in, const uint32_t timestamp_range)
{
    int32_t found_element = -1;
    uint32_t found_timestamp = 0;
    uint16_t start_entry = b->start;
    uint16_t current_element;

    for (int i = 0; i < tsb_size(b); i++) {
        current_element = (start_entry + i) % b->size;

        if ((((int64_t)b->timestamp[current_element]) >= ((int64_t)timestamp_in - (int64_t)timestamp_range))
                &&
                ((int64_t)b->timestamp[current_element] <= ((int64_t)timestamp_in + (int64_t)1))) {

            // timestamp of entry is in range
            if ((int64_t)b->timestamp[current_element] > (int64_t)found_timestamp) {

                // entry is newer than previous found entry, or is the first found entry
                found_timestamp = (uint32_t)b->timestamp[current_element];
                found_element = (int32_t)current_element;
            }
        }
    }

    if (found_element > -1) {

        // swap element with element in "start" position
        if (found_element != (int32_t)b->start) {
            void *p_save = b->data[found_element];
            uint64_t data_type_save = b->type[found_element];
            uint32_t timestamp_save = b->timestamp[found_element];

            b->data[found_element] = b->data[b->start];
            b->type[found_element] = b->type[b->start];
            b->timestamp[found_element] = b->timestamp[b->start];

            b->data[b->start] = p_save;
            b->type[b->start] = data_type_save;
            b->timestamp[b->start] = timestamp_save;
        }

        // fill data to return to caller
        *p = b->data[b->start];
        *data_type = b->type[b->start];
        *timestamp_out = b->timestamp[b->start];

        b->data[b->start] = NULL;
        b->timestamp[b->start] = 0;
        b->type[b->start] = 0;

        // change start element pointer
        b->start = (b->start + 1) % b->size;
        return true;
    }

    *p = NULL;
    return false;
}
#endif

bool tsb_read(TSBuffer *b, void **p, uint64_t *data_type, uint32_t *timestamp_out,
              const uint32_t timestamp_in, const uint32_t timestamp_range,
              uint16_t *removed_entries_back, uint16_t *is_skipping)
{
    *is_skipping = 0;

    // printf("tsb_read:000:%p size=%d st=%d end=%d tsin=%d tsrange=%d\n",
    //       (void *)b, b->size, b->start, b->end, timestamp_in, timestamp_range);

    if (tsb_empty(b) == true) {
        // printf("tsb_read:EMPTY:%p size=%d st=%d end=%d\n", (void *)b, b->size, b->start, b->end);
        *removed_entries_back = 0;
        *p = NULL;
        return false;
    }

    if ((int64_t)b->last_timestamp_out < ((int64_t)timestamp_in - (int64_t)timestamp_range)) {
        /* caller is missing a time range, either call more often, or increase range */
        *is_skipping = (timestamp_in - timestamp_range) - b->last_timestamp_out;
    }

    bool have_found_element = tsb_return_oldest_entry_in_range(b, p, data_type,
                              timestamp_out,
                              timestamp_in,
                              timestamp_range);

    // printf("tsb_read:%p size=%d st=%d end=%d have_found_element=%d\n", (void *)b, b->size, b->start, b->end,
    //       (int)have_found_element);

    if (have_found_element == true) {
        // only delete old entries if we found a "wanted" entry
        uint16_t removed_entries = tsb_delete_old_entries(b, ((int64_t)timestamp_in - (int64_t)timestamp_range));

        // printf("tsb_read:%p size=%d st=%d end=%d removed_entries=%d\n", (void *)b, b->size, b->start, b->end,
        //       (int)removed_entries);

        *removed_entries_back = removed_entries;

        // save the timestamp of the last read entry
        b->last_timestamp_out = *timestamp_out;
    } else {
        *removed_entries_back = 0;
    }

    return have_found_element;
}

TSBuffer *tsb_new(const int size)
{
    TSBuffer *buf = (TSBuffer *)calloc(1, sizeof(TSBuffer));

    if (!buf) {
        return NULL;
    }

    buf->size = size + 1; /* include empty elem */
    buf->start = 0;
    buf->end = 0;

    if (!(buf->data = (void **)calloc(buf->size, sizeof(void *)))) {
        free(buf);
        return NULL;
    }

    if (!(buf->type = (uint64_t *)calloc(buf->size, sizeof(uint64_t)))) {
        free(buf->data);
        free(buf);
        return NULL;
    }

    if (!(buf->timestamp = (uint32_t *)calloc(buf->size, sizeof(uint32_t)))) {
        free(buf->data);
        free(buf->type);
        free(buf);
        return NULL;
    }

    buf->last_timestamp_out = 0;

    // printf("tsb_new:%p size=%d st=%d end=%d\n", (void *)buf, buf->size, buf->start, buf->end);
    // tsb_debug_print_entries(buf);

    return buf;
}

void tsb_drain(TSBuffer *b)
{
    if (b) {
        // printf("tsb_drain:%p size=%d\n", (void *)b, tsb_size(b));
        // tsb_debug_print_entries(b);

        void *dummy = NULL;
        uint64_t dt;
        uint32_t to;
        uint16_t reb;
        uint16_t skip;

        while (tsb_read(b, &dummy, &dt, &to, UINT32_MAX, UINT32_MAX, &reb, &skip) == true) {
            // printf("tsb_drain:XX:%p data:%p\n", (void *)b, (void *)dummy);
            free(dummy);
        }

        // tsb_debug_print_entries(b);

        b->last_timestamp_out = 0;
    }
}

void tsb_kill(TSBuffer *b)
{
    if (b) {
        tsb_drain(b);

        free(b->data);
        free(b->type);
        free(b->timestamp);
        free(b);
    }
}

uint16_t tsb_size(const TSBuffer *b)
{
    if (tsb_empty(b) == true) {
        return 0;
    }

    return
        b->end > b->start ?
        b->end - b->start :
        (b->size - b->start) + b->end;
}




#if 0
static void tsb_debug_print_entries(const TSBuffer *b)
{
    uint16_t current_element;

    printf("tsb_debug_print_entries:---------------------\n");

    for (int i = 0; i < tsb_size(b); i++) {
        current_element = (b->start + i) % b->size;
        printf("tsb_debug_print_entries:loop=%d val=%d buf=%p\n",
               current_element, b->timestamp[current_element], (void *)b->data[current_element]);
    }

    printf("tsb_debug_print_entries:---------------------\n");
}

void unit_test()
{
#ifndef __MINGW32__
#include <time.h>
#endif

    printf("ts_buffer:testing ...\n");
    const int size = 5;
    const int bytes_per_entry = 200;

    TSBuffer *b1 = tsb_new(size);
    printf("b1=%p\n", b1);

    uint16_t size_ = tsb_size(b1);
    printf("size_:1=%d\n", size_);

#ifndef __MINGW32__
    srand(time(NULL));
#else
    // TODO: fixme ---
    srand(localtime());
    // TODO: fixme ---
#endif

    for (int j = 0; j < size + 0; j++) {
        void *tmp_b = calloc(1, bytes_per_entry);

        int val = rand() % 4999 + 1000;
        void *ret_p = tsb_write(b1, tmp_b, 1, val);
        printf("loop=%d val=%d\n", j, val);

        if (ret_p) {
            printf("kick oldest\n");
            free(ret_p);
        }

        size_ = tsb_size(b1);
        printf("size_:2=%d\n", size_);

    }

    size_ = tsb_size(b1);
    printf("size_:3=%d\n", size_);

    void *ptr;
    uint64_t dt;
    uint32_t to;
    uint32_t ti = 3000;
    uint32_t tr = 400;
    uint16_t reb = 0;
    uint16_t skip = 0;
    bool res1;

    bool loop = true;

    while (loop) {
        loop = false;
        ti = rand() % 4999 + 1000;
        tr = rand() % 100 + 1;
        res1 = tsb_read(b1, &ptr, &dt, &to, ti, tr, &reb, &skip);
        printf("ti=%d,tr=%d\n", (int)ti, (int)tr);

        if (res1 == true) {
            printf("found:ti=%d,tr=%d,TO=%d\n", (int)ti, (int)tr, (int)to);
            free(ptr);
            tsb_debug_print_entries(b1);
            break;
        } else if (tsb_size(b1) == 0) {
            break;
        }

        size_ = tsb_size(b1);
        printf("size_:4=%d\n", size_);
    }

    tsb_drain(b1);
    printf("drain\n");

    size_ = tsb_size(b1);
    printf("size_:99=%d\n", size_);

    tsb_kill(b1);
    b1 = NULL;
    printf("kill=%p\n", b1);
}

#endif

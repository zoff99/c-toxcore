/*
 * fuzz_static_toxutil.c
 *
 * libFuzzer harness that exposes internal static functions from toxutil.c
 * using the #define static trick.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* Expose all static functions/vars from the amalgamation */
#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

static int g_initialized = 0;

static void ensure_init(void)
{
    if (!g_initialized) {
        pthread_mutex_init(mutex_tox_util, NULL);
        g_initialized = 1;
    }
}

/*
 * Fuzz target 0: Internal linked-list operations
 */
static void fuzz_list_ops(const uint8_t *data, size_t size)
{
    if (size < 40) return;

    ensure_init();

    tox_utils_List list;
    tox_utils_list_init(&list);

    size_t pos = 0;

    /* Read a key (32 bytes) */
    uint8_t key[TOX_PUBLIC_KEY_SIZE];
    memcpy(key, data + pos, TOX_PUBLIC_KEY_SIZE);
    pos += TOX_PUBLIC_KEY_SIZE;

    /* Read key2 (4 bytes) */
    uint32_t key2;
    memcpy(&key2, data + pos, sizeof(key2));
    pos += sizeof(key2);

    /* Read data size (2 bytes), cap at 256 */
    uint16_t data_size_raw;
    memcpy(&data_size_raw, data + pos, sizeof(data_size_raw));
    pos += sizeof(data_size_raw);
    size_t data_size = (data_size_raw % 256) + 1;

    /* Allocate heap data (list_remove will free it) */
    uint8_t *heap_data = (uint8_t *)malloc(data_size);
    if (!heap_data) return;

    /* Fill with remaining fuzz data */
    size_t fill = (size - pos < data_size) ? (size - pos) : data_size;
    if (fill > 0) memcpy(heap_data, data + pos, fill);
    memset(heap_data + fill, 0, data_size - fill);

    /* Save a copy of the data BEFORE adding to list, because
     * tox_utils_list_remove will free the data pointer. */
    uint8_t *saved_copy = (uint8_t *)malloc(data_size);
    if (!saved_copy) {
        free(heap_data);
        return;
    }
    memcpy(saved_copy, heap_data, data_size);

    /* Exercise list operations */
    tox_utils_list_add(&list, key, key2, heap_data);
    /* heap_data ownership is now transferred to the list.
     * Do NOT use heap_data after this point. */
    heap_data = NULL;  /* prevent accidental use */

    /* Try to find it */
    tox_utils_Node *found = tox_utils_list_get(&list, key, key2);
    (void)found;

    /* Try with wrong key2 */
    uint32_t wrong_key2 = key2 ^ 0xFFFFFFFF;
    tox_utils_Node *not_found = tox_utils_list_get(&list, key, wrong_key2);
    (void)not_found;

    /* Remove by key+key2 — this frees the node AND its data */
    tox_utils_list_remove(&list, key, key2);

    /* Add again using the saved copy (NOT the freed heap_data) */
    uint8_t *heap_data2 = (uint8_t *)malloc(data_size);
    if (heap_data2) {
        memcpy(heap_data2, saved_copy, data_size);
        tox_utils_list_add(&list, key, key2, heap_data2);
        tox_utils_list_remove_2(&list, key);
    }

    /* Clear everything */
    tox_utils_list_clear(&list);

    free(saved_copy);
}

/*
 * Fuzz target 1: Capability packet parsing
 */
static void fuzz_capability_parsing(const uint8_t *data, size_t size)
{
    if (size < 3) return;

    ensure_init();

    if (size >= 3) {
        if ((data[0] == CAP_PACKET_ID) &&
            (data[1] == CAP_BYTE_0) &&
            (data[2] == CAP_BYTE_1)) {
            /* Valid capability packet detected */
        }
    }

    for (size_t trunc = 0; trunc < size && trunc < 5; trunc++) {
        if (trunc >= 3) {
            if ((data[0] == CAP_PACKET_ID) &&
                (data[1] == CAP_BYTE_0) &&
                (data[2] == CAP_BYTE_1)) {
                /* valid */
            }
        }
    }
}

/*
 * Fuzz target 2: check_file_signature
 */
static void fuzz_check_file_signature(const uint8_t *data, size_t size)
{
    if (size < TOX_PUBLIC_KEY_SIZE * 2) return;

    const uint8_t *key1 = data;
    const uint8_t *key2 = data + TOX_PUBLIC_KEY_SIZE;

    int result = check_file_signature(key1, key2, TOX_PUBLIC_KEY_SIZE);
    (void)result;

    for (size_t len = 1; len <= TOX_PUBLIC_KEY_SIZE; len += 7) {
        check_file_signature(key1, key2, len);
    }
}

/*
 * libFuzzer entry point
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;

    uint8_t selector = data[0] % 3;
    const uint8_t *payload = data + 1;
    size_t payload_size = size - 1;

    switch (selector) {
        case 0:
            fuzz_list_ops(payload, payload_size);
            break;
        case 1:
            fuzz_capability_parsing(payload, payload_size);
            break;
        case 2:
            fuzz_check_file_signature(payload, payload_size);
            break;
    }

    return 0;
}

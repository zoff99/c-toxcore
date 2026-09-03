// unit_test/static_test_toxutil_internals.c

#include "test_framework.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* Expose all static functions/vars from the amalgamation */
#define static
#include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#undef static

/*
 * The internal list functions in toxutil.c:
 *   - Always lock/unlock `mutex_tox_util` (must be initialized first)
 *   - `tox_utils_list_remove` calls free(n->data) unconditionally
 *     → callers must pass heap-allocated data (ownership transfer)
 */

/* Initialize the mutex exactly like tox_utils_new() does */
static void init_test_mutex(void)
{
    pthread_mutex_init(mutex_tox_util, NULL);
}

static void destroy_test_mutex(void)
{
    pthread_mutex_destroy(mutex_tox_util);
}

bool test_list_operations(void)
{
    init_test_mutex();

    tox_utils_List list;
    tox_utils_list_init(&list);
    T_ASSERT_INT_EQ(list.size, 0, "empty list size");
    T_ASSERT_PTR_NULL(list.head, "empty list head");

    uint8_t key[TOX_PUBLIC_KEY_SIZE];
    memset(key, 0xAA, sizeof(key));

    /* IMPORTANT: data must be heap-allocated. tox_utils_list_remove()
     * unconditionally calls free(n->data), so passing a stack pointer
     * would cause "free(): invalid pointer" abort. */
    int *heap_data = (int *)malloc(sizeof(int));
    T_ASSERT_PTR_NOT_NULL(heap_data, "malloc data failed");
    *heap_data = 42;

    tox_utils_list_add(&list, key, 123, heap_data);
    T_ASSERT_INT_EQ(list.size, 1, "size after add");

    tox_utils_Node *n = tox_utils_list_get(&list, key, 123);
    T_ASSERT_PTR_NOT_NULL(n, "node should be found");
    T_ASSERT_INT_EQ(*(int *)n->data, 42, "data should match");
    T_ASSERT_INT_EQ(n->key2, 123, "key2 should match");

    /* This calls free(n->data) and free(n) — safe because both were
     * heap-allocated. */
    tox_utils_list_remove(&list, key, 123);
    T_ASSERT_INT_EQ(list.size, 0, "size after remove");
    T_ASSERT_PTR_NULL(list.head, "head should be NULL after remove");

    destroy_test_mutex();
    return true;
}

bool test_list_multiple_entries(void)
{
    init_test_mutex();

    tox_utils_List list;
    tox_utils_list_init(&list);

    uint8_t key1[TOX_PUBLIC_KEY_SIZE];
    uint8_t key2[TOX_PUBLIC_KEY_SIZE];
    memset(key1, 0x11, sizeof(key1));
    memset(key2, 0x22, sizeof(key2));

    int *d1 = (int *)malloc(sizeof(int));
    int *d2 = (int *)malloc(sizeof(int));
    *d1 = 100;
    *d2 = 200;

    tox_utils_list_add(&list, key1, 1, d1);
    tox_utils_list_add(&list, key2, 2, d2);
    T_ASSERT_INT_EQ(list.size, 2, "size after two adds");

    /* Find by key + key2 */
    tox_utils_Node *n1 = tox_utils_list_get(&list, key1, 1);
    tox_utils_Node *n2 = tox_utils_list_get(&list, key2, 2);
    T_ASSERT_PTR_NOT_NULL(n1, "node 1 found");
    T_ASSERT_PTR_NOT_NULL(n2, "node 2 found");
    T_ASSERT_INT_EQ(*(int *)n1->data, 100, "node 1 data");
    T_ASSERT_INT_EQ(*(int *)n2->data, 200, "node 2 data");

    /* Non-existent lookup */
    uint8_t key_missing[TOX_PUBLIC_KEY_SIZE];
    memset(key_missing, 0xFF, sizeof(key_missing));
    tox_utils_Node *n_none = tox_utils_list_get(&list, key_missing, 99);
    T_ASSERT_PTR_NULL(n_none, "missing key should return NULL");

    /* Remove one */
    tox_utils_list_remove(&list, key1, 1);
    T_ASSERT_INT_EQ(list.size, 1, "size after removing one");

    /* Clear the rest */
    tox_utils_list_clear(&list);
    T_ASSERT_INT_EQ(list.size, 0, "size after clear");
    T_ASSERT_PTR_NULL(list.head, "head NULL after clear");

    destroy_test_mutex();
    return true;
}

bool test_list_remove_by_key_only(void)
{
    init_test_mutex();

    tox_utils_List list;
    tox_utils_list_init(&list);

    uint8_t key[TOX_PUBLIC_KEY_SIZE];
    memset(key, 0xBB, sizeof(key));

    /* Two entries with same key but different key2 */
    int *d1 = (int *)malloc(sizeof(int)); *d1 = 1;
    int *d2 = (int *)malloc(sizeof(int)); *d2 = 2;
    tox_utils_list_add(&list, key, 10, d1);
    tox_utils_list_add(&list, key, 20, d2);
    T_ASSERT_INT_EQ(list.size, 2, "size = 2");

    /* tox_utils_list_remove_2 removes ALL entries matching the key,
     * regardless of key2 */
    tox_utils_list_remove_2(&list, key);
    T_ASSERT_INT_EQ(list.size, 0, "all entries with that key removed");

    destroy_test_mutex();
    return true;
}

bool test_check_file_signature(void)
{
    uint8_t a[TOX_PUBLIC_KEY_SIZE];
    uint8_t b[TOX_PUBLIC_KEY_SIZE];

    memset(a, 0xCC, sizeof(a));
    memset(b, 0xCC, sizeof(b));
    T_ASSERT_INT_EQ(check_file_signature(a, b, TOX_PUBLIC_KEY_SIZE), 0,
                    "identical keys should return 0");

    b[0] = 0x00;
    T_ASSERT_INT_EQ(check_file_signature(a, b, TOX_PUBLIC_KEY_SIZE), 1,
                    "different keys should return 1");

    return true;
}

bool test_globals_initialized_to_zero(void)
{
    /* The static globals should be zero-initialized at program start */
    T_ASSERT_INT_EQ(global_friend_capability_list.size, 0,
                    "global capability list size = 0");
    T_ASSERT_PTR_NULL(global_friend_capability_list.head,
                      "global capability list head = NULL");
    T_ASSERT_INT_EQ(global_msgv2_incoming_ft_list.size, 0,
                    "global incoming FT list size = 0");
    T_ASSERT_INT_EQ(global_msgv2_outgoing_ft_list.size, 0,
                    "global outgoing FT list size = 0");
    T_ASSERT_INT_EQ(global_ts_ms, 0, "global_ts_ms = 0");
    return true;
}

int main(void)
{
    TEST_SUITE("Internal static functions (toxutil list ops)");
    RUN_TEST(test_list_operations);
    RUN_TEST(test_list_multiple_entries);
    RUN_TEST(test_list_remove_by_key_only);
    RUN_TEST(test_check_file_signature);
    RUN_TEST(test_globals_initialized_to_zero);
    SUITE_END();
    return test_summary("static_internals");
}

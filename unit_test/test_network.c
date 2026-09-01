#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* ────────────────────────────────────────────────────────────────
 * Forward declarations for internal functions from network.c
 * These are compiled into toxcore.o from the amalgamation.
 * We declare them here since they are not exposed in tox.h
 * ──────────────────────────────────────────────────────────────── */
size_t net_pack_u16(uint8_t *bytes, uint16_t v);
size_t net_pack_u32(uint8_t *bytes, uint32_t v);
size_t net_unpack_u16(const uint8_t *bytes, uint16_t *v);
size_t net_unpack_u32(const uint8_t *bytes, uint32_t *v);

/* ────────────────────────────────────────────────────────────────
 * Actual Tests against the REAL amalgamation code
 * ──────────────────────────────────────────────────────────────── */

bool test_net_pack_u32_basic() {
    uint8_t buf[4] = {0};
    size_t len = net_pack_u32(buf, 0x12345678);
    T_ASSERT_INT_EQ(len, 4, "pack length should be 4");
    T_ASSERT_INT_EQ(buf[0], 0x12, "byte 0");
    T_ASSERT_INT_EQ(buf[1], 0x34, "byte 1");
    T_ASSERT_INT_EQ(buf[2], 0x56, "byte 2");
    T_ASSERT_INT_EQ(buf[3], 0x78, "byte 3");
    return true;
}

bool test_net_unpack_u32_basic() {
    uint8_t buf[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t val = 0;
    size_t len = net_unpack_u32(buf, &val);
    T_ASSERT_INT_EQ(len, 4, "unpack length should be 4");
    T_ASSERT_INT_EQ(val, 0xDEADBEEF, "unpacked value");
    return true;
}

bool test_net_pack_unpack_roundtrip() {
    uint8_t buf[4] = {0};
    uint32_t original = 0xCAFEBABE;
    uint32_t unpacked = 0;
    
    net_pack_u32(buf, original);
    net_unpack_u32(buf, &unpacked);
    
    T_ASSERT_INT_EQ(unpacked, original, "roundtrip value");
    return true;
}

int main() {
    TEST_SUITE("network.c utility functions (via amalgamation)");
    RUN_TEST(test_net_pack_u32_basic);
    RUN_TEST(test_net_unpack_u32_basic);
    RUN_TEST(test_net_pack_unpack_roundtrip);
    SUITE_END();
    return test_summary("network");
}

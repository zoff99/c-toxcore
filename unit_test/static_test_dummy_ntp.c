/*
 * static_test_dummy_ntp.c
 *
 * Hardened static unit tests for dummy_ntp.c
 * Tests integer boundaries, extreme asymmetries, 32-bit wrap-around behavior,
 * and edge cases in the drift/jump logic.
 */

#include "test_framework.h"
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

/* Expose static functions and macros from the source file */
#define static
#include "../toxav/dummy_ntp.c"
#undef static

/* ── Offset & Roundtrip Calculations ─────────────────────────── */

bool test_dntp_calc_offset_symmetric(void)
{
    /* Local clock = 0, Remote clock = 100. Transit = 10ms both ways. */
    uint32_t t0 = 0;      /* local start */
    uint32_t t1 = 110;    /* remote start */
    uint32_t t2 = 115;    /* remote end */
    uint32_t t3 = 25;     /* local end */
    
    int64_t offset = dntp_calc_offset(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(offset, 100, "offset should be exactly 100ms");
    return true;
}

bool test_dntp_calc_offset_asymmetric(void)
{
    /* Local clock = 0, Remote clock = 100. Forward transit = 20ms, Backward = 10ms.
     * NTP offset calculation has a known bias of half the difference in transit times.
     * Bias = (20 - 10) / 2 = +5ms. Expected offset = 105ms. */
    uint32_t t0 = 0;
    uint32_t t1 = 120;
    uint32_t t2 = 125;
    uint32_t t3 = 35;
    
    int64_t offset = dntp_calc_offset(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(offset, 105, "offset should be 105ms (biased by asymmetric delay)");
    return true;
}

bool test_dntp_calc_roundtrip_delay(void)
{
    /* Transit 10ms both ways, remote processing 5ms. Total roundtrip = 20ms. */
    uint32_t t0 = 1000;
    uint32_t t1 = 1110;
    uint32_t t2 = 1115;
    uint32_t t3 = 1025;
    
    uint32_t rtt = dntp_calc_roundtrip_delay(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(rtt, 20, "roundtrip delay should be 20ms");
    return true;
}

bool test_dntp_offset_zero(void)
{
    uint32_t t0 = 500;
    uint32_t t1 = 500;
    uint32_t t2 = 510;
    uint32_t t3 = 510;
    
    int64_t offset = dntp_calc_offset(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(offset, 0, "identical clocks should yield 0 offset");
    
    uint32_t rtt = dntp_calc_roundtrip_delay(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(rtt, 0, "identical clocks should yield 0 rtt");
    return true;
}

bool test_dntp_offset_extreme_asymmetry(void)
{
    /* Forward trip = 1000ms, Backward trip = 0ms (theoretical extreme) */
    uint32_t t0 = 0;
    uint32_t t1 = 1100; /* remote start (offset 100, transit 1000) */
    uint32_t t2 = 1100; /* remote end (processing 0) */
    uint32_t t3 = 1000; /* local end (transit 0) */
    
    /* True offset = 100. Bias = 500. Calculated = 600. */
    int64_t offset = dntp_calc_offset(t1, t2, t0, t3);
    T_ASSERT_INT_EQ(offset, 600, "extreme asymmetry bias");
    return true;
}

bool test_dntp_roundtrip_underflow(void)
{
    /* remote processing time > local round trip time (bad data/clock drift) */
    uint32_t t0 = 0;
    uint32_t t1 = 10;
    uint32_t t2 = 100; /* remote diff = 90 */
    uint32_t t3 = 50;  /* local diff = 50 */
    
    uint32_t rtt = dntp_calc_roundtrip_delay(t1, t2, t0, t3);
    /* 50 - 90 = -40 -> wraps to 4294967256 */
    T_ASSERT_INT_EQ(rtt, (uint32_t)-40, "documents uint32_t underflow on bad data");
    return true;
}

bool test_dntp_offset_wraparound_32bit(void)
{
    /* local clock wraps around 32-bit boundary */
    uint32_t t0 = 0xFFFFFF00; /* local start */
    uint32_t t3 = 0x00000100; /* local end */
    uint32_t t1 = 0x000002E8; /* remote start */
    uint32_t t2 = 0x000004E8; /* remote end */
    
    int64_t offset = dntp_calc_offset(t1, t2, t0, t3);
    /* The code casts uint32_t to int64_t directly, so it DOES NOT handle 32-bit wraparound.
     * It computes a massively negative number. This test documents this limitation. */
    int64_t expected_buggy_offset = ((int64_t)t1 - (int64_t)t0 + (int64_t)t2 - (int64_t)t3) / 2;
    T_ASSERT_INT_EQ(offset, expected_buggy_offset, "documents lack of 32-bit wraparound handling");
    return true;
}

/* ── Drift / Jump Logic ──────────────────────────────────────── */

bool test_dntp_drift_null_ptr(void)
{
    bool jumped = dntp_drift(NULL, 100, 50, 10);
    T_ASSERT_FALSE(jumped, "should safely return false on NULL ptr");
    return true;
}

bool test_dntp_drift_jump_positive(void)
{
    int64_t current = 0;
    /* diff = 100, max = 50. 100 > 50 → JUMP */
    bool jumped = dntp_drift(&current, 100, 50, 10);
    T_ASSERT_TRUE(jumped, "should jump");
    T_ASSERT_INT_EQ(current, 100, "current should be updated to new_offset");
    return true;
}

bool test_dntp_drift_jump_negative(void)
{
    int64_t current = 50;
    /* diff = 100, max = 20. 100 > 20 → JUMP */
    bool jumped = dntp_drift(&current, -50, 20, 5);
    T_ASSERT_TRUE(jumped, "should jump");
    T_ASSERT_INT_EQ(current, -50, "current should be updated to new_offset");
    return true;
}

bool test_dntp_drift_up(void)
{
    int64_t current = 0;
    /* diff = 10, max = 50, jitter = 5. 10 <= 50, 10 > 5 → DRIFT +1 */
    bool jumped = dntp_drift(&current, 10, 50, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, 1, "current should drift up by 1");
    return true;
}

bool test_dntp_drift_down(void)
{
    int64_t current = 10;
    /* diff = 10, max = 50, jitter = 5. 10 <= 50, 10 > 5 → DRIFT -1 */
    bool jumped = dntp_drift(&current, 0, 50, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, 9, "current should drift down by 1");
    return true;
}

bool test_dntp_drift_within_jitter(void)
{
    int64_t current = 10;
    /* diff = 3, max = 50, jitter = 5. 3 <= 50, 3 <= 5 → NO DRIFT */
    bool jumped = dntp_drift(&current, 13, 50, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, 10, "current should not drift when within jitter");
    return true;
}

bool test_dntp_drift_exact_match(void)
{
    int64_t current = 42;
    /* diff = 0 → NO DRIFT */
    bool jumped = dntp_drift(&current, 42, 50, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, 42, "current should remain unchanged");
    return true;
}

bool test_dntp_drift_multiple_steps(void)
{
    int64_t current = 0;
    
    /* Target is 10. Max drift 50, jitter 2. */
    /* Step 1: diff 10 > 2 → current = 1 */
    dntp_drift(&current, 10, 50, 2);
    T_ASSERT_INT_EQ(current, 1, "step 1 drift");
    
    /* Step 2: diff 9 > 2 → current = 2 */
    dntp_drift(&current, 10, 50, 2);
    T_ASSERT_INT_EQ(current, 2, "step 2 drift");
    
    /* Fast forward to diff = 2 */
    current = 8;
    /* Step: diff 2 <= 2 → NO DRIFT */
    dntp_drift(&current, 10, 50, 2);
    T_ASSERT_INT_EQ(current, 8, "should stop drifting when within jitter threshold");
    
    return true;
}

bool test_dntp_drift_negative_offsets(void)
{
    int64_t current = -50;
    
    /* Target is -40. diff = 10. Max = 100, jitter = 5.
     * 10 <= 100, 10 > 5 → DRIFT +1 */
    bool jumped = dntp_drift(&current, -40, 100, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, -49, "should drift towards -40 (upwards)");
    
    /* Target is -60. diff = 11. Max = 100, jitter = 5.
     * 11 <= 100, 11 > 5 → DRIFT -1 */
    jumped = dntp_drift(&current, -60, 100, 5);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, -50, "should drift towards -60 (downwards)");
    return true;
}

bool test_dntp_drift_max_zero(void)
{
    int64_t current = 0;
    /* max_offset = 0. Any diff > 0 should jump. */
    bool jumped = dntp_drift(&current, 1, 0, 0);
    T_ASSERT_TRUE(jumped, "should jump when max_offset is 0 and diff > 0");
    T_ASSERT_INT_EQ(current, 1, "current updated");
    return true;
}

bool test_dntp_drift_jitter_zero(void)
{
    int64_t current = 0;
    /* diff = 5, max = 10, jitter = 0. abs_value (5) > jitter (0) -> drifts. */
    bool jumped = dntp_drift(&current, 5, 10, 0);
    T_ASSERT_FALSE(jumped, "should not jump");
    T_ASSERT_INT_EQ(current, 1, "should drift by 1");
    return true;
}

bool test_dntp_drift_large_values(void)
{
    int64_t current = -1000000000LL;
    int64_t target =  1000000000LL;
    int64_t max_drift = 500000000LL;
    
    /* diff = 2,000,000,000 > max_drift -> JUMP */
    bool jumped = dntp_drift(&current, target, max_drift, 100);
    T_ASSERT_TRUE(jumped, "should jump on large diff");
    T_ASSERT_INT_EQ(current, target, "current updated to target");
    return true;
}

bool test_dntp_drift_oscillation(void)
{
    int64_t current = 10;
    /* Target = 12. max = 100, jitter = 2. */
    dntp_drift(&current, 12, 100, 2); // diff 2 <= 2 -> no drift
    T_ASSERT_INT_EQ(current, 10, "stays at 10");
    
    /* Target shifts to 8. diff 2 <= 2 -> no drift */
    dntp_drift(&current, 8, 100, 2);
    T_ASSERT_INT_EQ(current, 10, "stays at 10");
    return true;
}

bool test_dntp_drift_boundary_exact_max(void)
{
    int64_t current = 0;
    /* diff = 50, max = 50. The code uses `abs_value > max_offset_for_drift`.
     * Since 50 > 50 is false, it should NOT jump, but drift. */
    bool jumped = dntp_drift(&current, 50, 50, 10);
    T_ASSERT_FALSE(jumped, "should not jump when diff == max_offset");
    T_ASSERT_INT_EQ(current, 1, "should drift by 1");
    return true;
}

int main(void)
{
    TEST_SUITE("Dummy NTP hardened static tests");
    
    /* offset & delay calculations */
    RUN_TEST(test_dntp_calc_offset_symmetric);
    RUN_TEST(test_dntp_calc_offset_asymmetric);
    RUN_TEST(test_dntp_calc_roundtrip_delay);
    RUN_TEST(test_dntp_offset_zero);
    RUN_TEST(test_dntp_offset_extreme_asymmetry);
    RUN_TEST(test_dntp_roundtrip_underflow);
    RUN_TEST(test_dntp_offset_wraparound_32bit);
    
    /* drift / jump logic */
    RUN_TEST(test_dntp_drift_null_ptr);
    RUN_TEST(test_dntp_drift_jump_positive);
    RUN_TEST(test_dntp_drift_jump_negative);
    RUN_TEST(test_dntp_drift_up);
    RUN_TEST(test_dntp_drift_down);
    RUN_TEST(test_dntp_drift_within_jitter);
    RUN_TEST(test_dntp_drift_exact_match);
    RUN_TEST(test_dntp_drift_multiple_steps);
    RUN_TEST(test_dntp_drift_negative_offsets);
    RUN_TEST(test_dntp_drift_max_zero);
    RUN_TEST(test_dntp_drift_jitter_zero);
    RUN_TEST(test_dntp_drift_large_values);
    RUN_TEST(test_dntp_drift_oscillation);
    RUN_TEST(test_dntp_drift_boundary_exact_max);
    
    SUITE_END();
    return test_summary("dummy_ntp_hardened");
}

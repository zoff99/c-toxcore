#include "test_framework.h"
#include "../../toxcore/tox.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "../mid_roster.h"

extern Tox *create_dummy_tox(void);

#define MID_MSG_PRESENCE     1
#define MID_MSG_ROSTER_REQUEST 2

bool test_custom_packet_null_and_zero(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    mid_on_group_self_join(s, tox, 1, (uint8_t*)"test", 4);
    
    mid_on_group_custom_packet(s, tox, 1, 1, NULL, 0);
    mid_on_group_custom_packet(s, tox, 1, 1, NULL, 100);
    mid_on_group_custom_packet(s, tox, 1, 1, (uint8_t*)"", 0);
    mid_free(s);
    return true;
}

bool test_custom_packet_malformed(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    mid_on_group_self_join(s, tox, 1, (uint8_t*)"test", 4);
    
    uint8_t bad_magic[5] = {0x00, 0x00, 0x00, 0x00, MID_MSG_PRESENCE};
    mid_on_group_custom_packet(s, tox, 1, 1, bad_magic, sizeof(bad_magic));
    
    uint8_t short_pkt[4] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION};
    mid_on_group_custom_packet(s, tox, 1, 1, short_pkt, sizeof(short_pkt));
    
    uint8_t small_presence[66] = {0};
    small_presence[0] = MID_MAGIC_0;
    small_presence[1] = MID_MAGIC_1;
    small_presence[2] = MID_MAGIC_2;
    small_presence[3] = MID_PROTOCOL_VERSION;
    small_presence[4] = MID_MSG_PRESENCE;
    mid_on_group_custom_packet(s, tox, 1, 1, small_presence, sizeof(small_presence));
    
    uint8_t huge_pkt[1200] = {MID_MAGIC_0, MID_MAGIC_1, MID_MAGIC_2, MID_PROTOCOL_VERSION, MID_MSG_PRESENCE};
    mid_on_group_custom_packet(s, tox, 1, 1, huge_pkt, sizeof(huge_pkt));
    
    mid_free(s);
    return true;
}

bool test_corrupt_signature(void) {
    MidState *s = mid_new(NULL, NULL, 0);
    Tox *tox = create_dummy_tox();
    mid_on_group_self_join(s, tox, 1, (uint8_t*)"test", 4);
    
    uint8_t pkt[5 + 64 + 75] = {0};
    pkt[0] = MID_MAGIC_0;
    pkt[1] = MID_MAGIC_1;
    pkt[2] = MID_MAGIC_2;
    pkt[3] = MID_PROTOCOL_VERSION;
    pkt[4] = MID_MSG_PRESENCE;
    
    uint8_t *body = pkt + 5 + 64;
    body[0] = 0;
    body[9] = 0;
    body[10] = 0;
    memset(body + 11, 0xCC, 32);
    memset(body + 11 + 32, 0xCC, 32);
    
    mid_on_group_custom_packet(s, tox, 1, 1, pkt, sizeof(pkt));
    
    mid_free(s);
    return true;
}

int main(void) {
    TEST_SUITE("midroster adversarial / security tests");

    RUN_TEST(test_custom_packet_null_and_zero);
    RUN_TEST(test_custom_packet_malformed);
    RUN_TEST(test_corrupt_signature);

    SUITE_END();
    return test_summary("midroster_adversarial");
}

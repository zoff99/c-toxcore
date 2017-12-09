/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2016-2018 The TokTok team.
 * Copyright © 2013-2015 Tox project.
 */
#ifndef C_TOXCORE_TOXAV_BWCONTROLLER_H
#define C_TOXCORE_TOXAV_BWCONTROLLER_H

#include <stdint.h>

#include "../toxcore/Messenger.h"

#ifndef TOX_DEFINED
#define TOX_DEFINED
typedef struct Tox Tox;
#endif /* TOX_DEFINED */

typedef struct BWController_s BWController;

typedef void m_cb(BWController *bwc, uint32_t friend_number, float todo, void *user_data);

BWController *bwc_new(Tox *t, uint32_t friendnumber, m_cb *mcb, void *mcb_user_data, Mono_Time *toxav_given_mono_time);

void bwc_kill(BWController *bwc);

void bwc_feed_avg(BWController *bwc, uint32_t bytes);
void bwc_add_lost(BWController *bwc, uint32_t bytes);
void bwc_add_lost_v3(BWController *bwc, uint32_t bytes);
void bwc_add_recv(BWController *bwc, uint32_t bytes);

#endif // C_TOXCORE_TOXAV_BWCONTROLLER_H

#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

cd ..

echo '

#include <alloca.h>
#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>


#include <sodium.h>

#include <libavcodec/avcodec.h>
#include <libavutil/common.h>

#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>

#include <opus.h>

' > amalgamation/toxcore_amalgamation.c

cat \
toxcore/ccompat.h \
\
toxcore/attributes.h \
toxcore/logger.h \
toxcore/mono_time.h \
toxcore/crypto_core.h \
toxcore/network.h \
toxcore/DHT.h \
toxcore/shared_key_cache.h \
toxcore/onion.h \
toxcore/forwarding.h \
toxcore/TCP_client.h \
toxcore/TCP_connection.h \
toxcore/net_crypto.h \
toxcore/onion_client.h \
toxcore/TCP_common.h \
toxcore/TCP_server.h \
\
toxcore/group_moderation.h \
toxcore/group_common.h \
toxcore/group_announce.h \
toxcore/onion_announce.h \
toxcore/group_onion_announce.h \
toxcore/state.h \
\
third_party/cmp/*.h \
\
toxcore/bin_pack.h \
toxcore/bin_unpack.h \
\
toxencryptsave/*.h \
\
toxcore/tox.h \
toxcore/tox_events.h \
toxcore/events/*.h \
\
toxcore/announce.h \
\
\
toxcore/friend_connection.h \
toxcore/friend_requests.h \
toxcore/group_chats.h \
toxcore/group_connection.h \
toxcore/group.h \
toxcore/group_pack.h \
toxcore/LAN_discovery.h \
toxcore/list.h \
toxcore/Messenger.h \
toxcore/ping_array.h \
toxcore/ping.h \
toxcore/timed_auth.h \
toxcore/tox_dispatch.h \
toxcore/tox_private.h \
toxcore/tox_struct.h \
toxcore/tox_unpack.h \
toxcore/util.h \
\
toxutil/toxutil.h \
\
toxav/ring_buffer.h \
toxav/bwcontroller.h \
toxav/msi.h \
toxav/rtp.h \
toxav/ts_buffer.h \
toxav/toxav_hacks.h \
toxav/groupav.h \
toxav/toxav.h \
toxav/video.h \
toxav/audio.h \
toxav/tox_generic.h \
toxav/codecs/toxav_codecs.h \
\
    toxcore/*.c toxcore/*/*.c toxencryptsave/*.c \
    toxav/*.c toxav/codecs/*/*.c third_party/cmp/*.c \
    toxutil/toxutil.c \
    |grep -v '#include "' >> amalgamation/toxcore_amalgamation.c
#
#
#
echo '

#include <alloca.h>
#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#include <sodium.h>

' > amalgamation/toxcore_amalgamation_no_toxav.c

cat \
toxcore/ccompat.h \
\
toxcore/attributes.h \
toxcore/logger.h \
toxcore/mono_time.h \
toxcore/crypto_core.h \
toxcore/network.h \
toxcore/DHT.h \
toxcore/shared_key_cache.h \
toxcore/onion.h \
toxcore/forwarding.h \
toxcore/TCP_client.h \
toxcore/TCP_connection.h \
toxcore/net_crypto.h \
toxcore/onion_client.h \
toxcore/TCP_common.h \
toxcore/TCP_server.h \
\
toxcore/group_moderation.h \
toxcore/group_common.h \
toxcore/group_announce.h \
toxcore/onion_announce.h \
toxcore/group_onion_announce.h \
toxcore/state.h \
\
third_party/cmp/*.h \
\
toxcore/bin_pack.h \
toxcore/bin_unpack.h \
\
toxencryptsave/*.h \
\
toxcore/tox.h \
toxcore/tox_events.h \
toxcore/events/*.h \
\
toxcore/announce.h \
\
\
toxcore/friend_connection.h \
toxcore/friend_requests.h \
toxcore/group_chats.h \
toxcore/group_connection.h \
toxcore/group.h \
toxcore/group_pack.h \
toxcore/LAN_discovery.h \
toxcore/list.h \
toxcore/Messenger.h \
toxcore/ping_array.h \
toxcore/ping.h \
toxcore/timed_auth.h \
toxcore/tox_dispatch.h \
toxcore/tox_private.h \
toxcore/tox_struct.h \
toxcore/tox_unpack.h \
toxcore/util.h \
\
toxutil/toxutil.h \
\
    toxcore/*.c toxcore/*/*.c toxencryptsave/*.c \
    third_party/cmp/*.c \
    toxutil/toxutil.c \
    |grep -v '#include "' >> amalgamation/toxcore_amalgamation_no_toxav.c
#
#
#
ls -hal amalgamation/toxcore_amalgamation.c
ls -hal amalgamation/toxcore_amalgamation_no_toxav.c
#
#
gcc -O3 -fPIC amalgamation/toxcore_amalgamation.c \
    $(pkg-config --cflags --libs libsodium opus vpx libavcodec libavutil x264) -pthread \
    -c -o amalgamation/libtoxcore.o
#
ls -hal amalgamation/libtoxcore.o
#
#
ar rcs amalgamation/libtoxcore.a amalgamation/libtoxcore.o
#
ls -hal amalgamation/libtoxcore.a


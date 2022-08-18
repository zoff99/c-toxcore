#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

cd ..

tcc -Dinline=static -o send_message_test -Wall -Werror -bench \
 -g auto_tests/auto_test_support.c auto_tests/send_message_test.c \
 testing/misc_tools.c toxav/*.c toxcore/*.c toxcore/*/*.c \
 toxencryptsave/*.c third_party/cmp/*.c toxav/codecs/*/*.c \
 $(pkg-config --cflags --libs libsodium opus vpx libavcodec x264)

#! /bin/bash

pwd
id -a

echo $ZOXCORE_CT
echo $TOKTOK_CT

ls -al $ZOXCORE_CT
ls -al $TOKTOK_CT


export PKG_CONFIG_PATH="$ZOXCORE_CT"/lib/pkgconfig
export LD_LIBRARY_PATH="$ZOXCORE_CT"/lib
echo $PKG_CONFIG_PATH
echo $LD_LIBRARY_PATH
pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium
pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium

rm -f z_sender
gcc -O0 -fPIC z_sender.c -fsanitize=address -static-libasan $(pkg-config --cflags --libs toxcore libavcodec libavutil x264 opus vpx libsodium) -pthread -o z_sender || exit 1
ldd z_sender
ls -al z_sender
./z_sender &

sleep 20

cat ./z_sender_toxid.txt;echo
toxid_sender=$(cat ./z_sender_toxid.txt)

export PKG_CONFIG_PATH="$TOKTOK_CT"/lib/pkgconfig
export LD_LIBRARY_PATH="$TOKTOK_CT"/lib
echo $PKG_CONFIG_PATH
echo $LD_LIBRARY_PATH
pkg-config --cflags toxcore opus vpx libsodium
pkg-config --libs toxcore opus vpx libsodium

rm -f toktok_receiver
gcc -O0 -fPIC toktok_receiver.c -fsanitize=address -static-libasan $(pkg-config --cflags --libs toxcore opus vpx libsodium) -pthread -o toktok_receiver || exit 1
ldd toktok_receiver
ls -al toktok_receiver
./toktok_receiver "$toxid_sender"



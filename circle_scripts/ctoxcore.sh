#! /bin/bash

_HOME_="$(pwd)"
export _HOME_

cd "$1"
_CTC_SRC_DIR_="$(pwd)"
export _CTC_SRC_DIR_

export _SRC_=$_HOME_/src/
export _INST_=$_HOME_/inst/


export CF2=" -O3 -g"
export CF3=" "
export VV1=" VERBOSE=1 V=1 "


mkdir -p $_SRC_
mkdir -p $_INST_

export LD_LIBRARY_PATH=$_INST_/lib/
export PKG_CONFIG_PATH=$_INST_/lib/pkgconfig

cd "$_CTC_SRC_DIR_"/
pwd
ls -al


sed -i -e 'sm#define DISABLE_H264_ENCODER_FEATURE.*m#define DISABLE_H264_ENCODER_FEATURE 1m' toxav/rtp.c
cat toxav/rtp.c |grep 'define DISABLE_H264_ENCODER_FEATURE'

./autogen.sh
make clean
export CFLAGS=" $CF2 -D_GNU_SOURCE -I$_INST_/include/ -O3 -g -fstack-protector-all "
export LDFLAGS=-L$_INST_/lib

./configure \
--prefix=$_INST_ \
--disable-soname-versions --disable-testing --disable-shared
make -j$(nproc) || exit 1
make install

export CFLAGS=" $CFLAGS -fPIC "
export CXXFLAGS=" $CFLAGS -fPIC "
export LDFLAGS=" $LDFLAGS -fPIC "
make V=1 -j$(nproc) check || exit 1


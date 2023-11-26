#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

if [ "$1""x" == "buildx" ]; then
    docker build -f Dockerfile_ffl_ub20 -t ctoxcore_001_ffl_ub20 .
    exit 0
fi

# docker info

mkdir -p $_HOME_/artefacts
mkdir -p $_HOME_/script
mkdir -p $_HOME_/workspace

echo '#! /bin/bash

## ----------------------
numcpus_=$(nproc)
quiet_=1
## ----------------------

echo "hello"

export qqq=""
export TEST_MAX_TIME=$[10*60] # 10 minutes

if [ "$quiet_""x" == "1x" ]; then
	export qqq=" -qq "
fi


redirect_cmd() {
    if [ "$quiet_""x" == "1x" ]; then
        "$@" > /dev/null 2>&1
    else
        "$@"
    fi
}

echo ""
echo ""
echo "--------------------------------"
echo "clang version:"
c++ --version
echo "--------------------------------"
echo ""
echo ""

cd /c-toxcore

rm -Rf /workspace/_build/
rm -Rf /workspace/auto_tests/
rm -Rf /workspace/cmake/
rm -f  /workspace/CMakeLists.txt
rm -Rfv /workspace/custom_tests/

echo "make a local copy ..."
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/

cd /workspace/

/etc/init.d/tor restart
ps -ef|grep tor
# cat /usr/share/tor/tor-service-defaults-torrc
# curl -x socks5h://localhost:9050 -s https://check.torproject.org/api/ip

echo "#########################"
echo "           IP"
echo "#########################"
ip addr
echo "#########################"
echo "#########################"

mkdir -p "$PWD"/_install/
cp -a /workspace2/build/inst/* "$PWD"/_install/

type llvm-symbolizer


export PKG_CONFIG_PATH="$PWD"/_install/lib/pkgconfig
echo $PKG_CONFIG_PATH

pkg-config --variable pc_path pkg-config
pkg-config --cflags libavcodec libavutil
pkg-config --libs libavcodec libavutil

CC=clang cmake -B_build -H. -GNinja \
    -DCMAKE_INSTALL_PREFIX:PATH="$PWD/_install" \
    -DCMAKE_C_FLAGS="-g -O0 -Wno-everything -fkeep-inline-functions -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=thread" \
    -DCMAKE_CXX_FLAGS="-g -O0 -Wno-everything -fkeep-inline-functions -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=thread" \
    -DCMAKE_EXE_LINKER_FLAGS="-g -O0 -Wno-everything -fkeep-inline-functions -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=thread" \
    -DCMAKE_SHARED_LINKER_FLAGS="-g -O0 -Wno-everything -fkeep-inline-functions -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=thread" \
    -DMIN_LOGGER_LEVEL=INFO \
    -DMUST_BUILD_TOXAV=ON \
    -DNON_HERMETIC_TESTS=OFF \
    -DSTRICT_ABI=OFF \
    -DUSE_IPV6=OFF \
    -DAUTOTEST=OFF \
    -DBUILD_MISC_TESTS=OFF \
    -DBUILD_FUN_UTILS=OFF
cd _build
ninja install -j"$(nproc)" || exit 1

cd /workspace/
pwd
ls -1 ./custom_tests/*.c
export PKG_CONFIG_PATH="$PWD"/_install/lib/pkgconfig
export LD_LIBRARY_PATH="$PWD"/_install/lib
echo $PKG_CONFIG_PATH
echo $LD_LIBRARY_PATH
pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium
pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium

# HINT: for local runs use *.c and *.l as source files
for i in $(ls -1 ./custom_tests/*.l) ; do
    mv -v "$i" "$i".c
done

echo "race:/workspace2/build/FFmpeg-n6.1/libavutil/*.c" > /workspace/tsan_ignore
# echo "race:2110_toxav_ts_sync_test.c:1309" >> /workspace/tsan_ignore
echo "" >> /workspace/tsan_ignore

cat /workspace/tsan_ignore

for i in $(ls -1 ./custom_tests/*.c) ; do
    echo "CCC:--------------- ""$i"" ---------------"
    rm -f test
    clang -g -O1 -fno-omit-frame-pointer -fkeep-inline-functions -fsanitize=thread \
    -Wno-everything -Wno-missing-variable-declarations \
    $(pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium) \
    $(pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium) \
    "$i" \
    -o test
    echo "RUN:--------------- ""$i"" ---------------"
    export TSAN_OPTIONS="color=always"
    export TSAN_OPTIONS="$TSAN_OPTIONS,halt_on_error=1"
    export TSAN_OPTIONS="$TSAN_OPTIONS,second_deadlock_stack=1"
    export TSAN_OPTIONS="$TSAN_OPTIONS,symbolize=1"
    export TSAN_OPTIONS="$TSAN_OPTIONS,suppressions=/workspace/tsan_ignore"
    ./test
    if [ $? -ne 0 ]; then
        echo "ERR:--------------- ""$i"" ---------------"
        exit $?
    else
        echo "OK :*************** ""$i"" ***************"
    fi
done

mkdir -p /artefacts/custom_tests/
chmod a+rwx -R /workspace/
chmod a+rwx -R /artefacts/

' > $_HOME_/script/do_it___external.sh

chmod a+rx $_HOME_/script/do_it___external.sh


system_to_build_for="ctoxcore_001_ffl_ub20"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/sh -c "/bin/bash /script/do_it___external.sh"


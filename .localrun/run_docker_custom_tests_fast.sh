#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

if [ "$1""x" == "buildx" ]; then
    docker build -f Dockerfile_ub20 -t ctoxcore_001_ub20 .
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

CC=clang cmake -B_build -H. -GNinja \
    -DCMAKE_INSTALL_PREFIX:PATH="$PWD/_install" \
    -DCMAKE_C_FLAGS="-g -O1 -Wno-everything -fsanitize-recover=address -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_CXX_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DMIN_LOGGER_LEVEL=DEBUG \
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

clang flakynet.c -o f.so -fPIC -ldl -shared
ls -al f.so

ls -1 ./custom_tests/*.c
export PKG_CONFIG_PATH="$PWD"/_install/lib/pkgconfig

export ASAN_OPTIONS=halt_on_error=0

export LD_LIBRARY_PATH="$PWD"/_install/lib
echo $PKG_CONFIG_PATH
echo $LD_LIBRARY_PATH
pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium
pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium

# HINT: for local runs use *.c and *.l as source files
for i in $(ls -1 ./custom_tests/*.l) ; do
    mv -v "$i" "$i".c
done

for i in $(ls -1 ./custom_tests/*.c) ; do
    echo "CCC:--------------- ""$i"" ---------------"
    rm -f test
    clang -g -O2 -fno-omit-frame-pointer -fsanitize=address -fsanitize-recover=address \
    -Wno-everything -Wno-missing-variable-declarations \
    $(pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium) \
    $(pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium) \
    "$i" \
    -o test
    echo "RUN:--------------- ""$i"" ---------------"
    # LD_PRELOAD=/workspace/f.so ./test
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


system_to_build_for="ctoxcore_001_ub20"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/sh -c "/bin/bash /script/do_it___external.sh"


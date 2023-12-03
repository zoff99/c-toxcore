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

#ls -al /workspace/group_chats.c

mkdir -p /workspace/toktok/
mkdir -p /workspace/toktok/_install/
cd /workspace/toktok/
git clone https://github.com/TokTok/c-toxcore
cd c-toxcore/
git checkout 7cfe35dff2209f09ca4a08433a7f16b09e8683f3
cp -av /workspace/third_party/cmp/* third_party/cmp/


#ls -al /workspace/toktok/c-toxcore/toxcore/group_chats.c
#cp -av /workspace/group_chats.c /workspace/toktok/c-toxcore/toxcore/
#ls -al /workspace/toktok/c-toxcore/toxcore/group_chats.c

CC=clang cmake -B_build -H. -GNinja \
    -DCMAKE_INSTALL_PREFIX:PATH="/workspace/toktok/_install" \
    -DCMAKE_C_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_CXX_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
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

ls -al /workspace/toktok/_install/
ls -al /workspace/toktok/_install/lib/

cd /workspace/

CC=clang cmake -B_build -H. -GNinja \
    -DCMAKE_INSTALL_PREFIX:PATH="$PWD/_install" \
    -DCMAKE_C_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_CXX_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_EXE_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
    -DCMAKE_SHARED_LINKER_FLAGS="-g -O1 -Wno-everything -Wno-missing-variable-declarations -fno-omit-frame-pointer -fsanitize=address" \
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

echo "##########################"
echo "##########################"
ls -al /workspace/_install/lib/libtoxcore.a
ls -al /workspace/_install/include/tox/tox.h
ls -al /workspace/_install/lib/pkgconfig/toxcore.pc
export ZOXCORE_CT="/workspace/_install"
echo "##########################"
ls -al /workspace/toktok/_install/lib/libtoxcore.a
ls -al /workspace/toktok/_install/include/tox/tox.h
ls -al /workspace/toktok/_install/lib/pkgconfig/toxcore.pc
export TOKTOK_CT="/workspace/toktok/_install"
echo "##########################"
echo "##########################"

cd /workspace/
pwd
ls -1 ./cross_custom_tests/
export PKG_CONFIG_PATH="$PWD"/_install/lib/pkgconfig
export LD_LIBRARY_PATH="$PWD"/_install/lib
echo $PKG_CONFIG_PATH
echo $LD_LIBRARY_PATH
pkg-config --cflags toxcore libavcodec libavutil x264 opus vpx libsodium
pkg-config --libs toxcore libavcodec libavutil x264 opus vpx libsodium

cd /workspace/cross_custom_tests/
for i in $(ls -1 ./) ; do
    echo "$i"
    cd $i/
    ./test.sh
    cd /workspace/cross_custom_tests/
done

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


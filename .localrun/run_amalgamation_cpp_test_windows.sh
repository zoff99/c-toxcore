#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

system_to_build_for="windows"

mkdir -p $_HOME_/"$system_to_build_for"/

mkdir -p $_HOME_/"$system_to_build_for"/artefacts
mkdir -p $_HOME_/"$system_to_build_for"/script
mkdir -p $_HOME_/"$system_to_build_for"/workspace

ls -al $_HOME_/"$system_to_build_for"/

rsync -a ../ --exclude=.localrun $_HOME_/"$system_to_build_for"/workspace/build
chmod a+rwx -R $_HOME_/"$system_to_build_for"/workspace/build

echo '#! /bin/bash

export ARCH=x86_64
export WINEARCH=win64

apt-get update && apt-get install -y --no-install-recommends \
                   autoconf \
                   automake \
                   build-essential \
                   ca-certificates \
                   cmake \
                   extra-cmake-modules \
                   git \
                   libarchive-tools \
                   libtool \
                   nsis \
                   pkg-config \
                   texinfo \
                   unzip \
                   wget \
                   yasm \
                   zip \
                   wine \
                   g++-mingw-w64-${ARCH//_/-} \
                   gcc-mingw-w64-${ARCH//_/-}

update-alternatives --set ${ARCH}-w64-mingw32-gcc /usr/bin/${ARCH}-w64-mingw32-gcc-posix && \
    update-alternatives --set ${ARCH}-w64-mingw32-g++ /usr/bin/${ARCH}-w64-mingw32-g++-posix

mkdir -p /workspace/build/inst/

# ------------ libsodium ------------
# ------------ libsodium ------------
mkdir -p /workspace/sodium/
cd /workspace/
cd sodium/

rm -f libsodium.tgz
rm -Rf libsodium-*
SODIUM_VERSION=1.0.19
wget "https://github.com/jedisct1/libsodium/releases/download/${SODIUM_VERSION}-RELEASE/libsodium-${SODIUM_VERSION}.tar.gz" -O libsodium.tgz
tar -xzvf libsodium.tgz
cd libsodium-*

MINGW_ARCH="x86_64"
HOST_OPTION="--host=${MINGW_ARCH}-w64-mingw32"
CROSS_LDFLAG=""
CROSS_CFLAG=""
CROSS_CPPFLAG=""
export PKG_CONFIG_PATH="/workspace/build/inst/lib/pkgconfig"

CFLAGS="${CROSS_CFLAG}" \
LDFLAGS="${CROSS_LDFLAG} -fstack-protector" \
  ./configure "${HOST_OPTION}" \
              "--prefix=/workspace/build/inst/" \
              --disable-shared \
              --enable-static

make -j $(nproc)
make install
# ------------ libsodium ------------
# ------------ libsodium ------------

cd /workspace/build/
pwd
id -a
cat /etc/os-release|grep PRETTY_NAME||echo ""
echo $(pkg-config --cflags --libs libsodium)

cd amalgamation/
${ARCH}-w64-mingw32-g++ -static -O3 -fpermissive amalgamation_cpp_test.cpp $(pkg-config --cflags --libs libsodium) -lwinpthread -lwsock32 -lws2_32 -liphlpapi -o amalgamation_cpp_test
ls -al amalgamation_cpp_test.exe
wine ./amalgamation_cpp_test.exe

cp ./amalgamation_cpp_test.exe /artefacts/amalgamation_cpp_test.exe
chmod a+rwx /artefacts/*

' > $_HOME_/"$system_to_build_for"/script/run.sh

docker run -ti --rm \
  -v $_HOME_/"$system_to_build_for"/artefacts:/artefacts \
  -v $_HOME_/"$system_to_build_for"/script:/script \
  -v $_HOME_/"$system_to_build_for"/workspace:/workspace \
  --net=host \
  ubuntu:22.04 \
  /bin/bash /script/run.sh

if [ $? -ne 0 ]; then
    echo "** ERROR **:$system_to_build_for_orig"
    exit 1
else
    echo "--SUCCESS--:$system_to_build_for_orig"
fi




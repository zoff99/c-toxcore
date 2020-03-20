#! /bin/bash

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

echo $_HOME_
cd $_HOME_

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


echo "installing system packages ..."

redirect_cmd apt-get update $qqq

redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends lsb-release
system__=$(lsb_release -i|cut -d ':' -f2|sed -e "s#\s##g")
version__=$(lsb_release -r|cut -d ':' -f2|sed -e "s#\s##g")
echo "compiling on: $system__ $version__"

echo "installing more system packages ..."

pkgs="
    git
    ca-certificates
    zlib1g-dev
    wget
    unzip
    zip
    rsync
    clang
    cmake
    libconfig-dev
    libgtest-dev
    libopus-dev
    libsodium-dev
    libvpx-dev
    ninja-build
    pkg-config
"

for i in $pkgs ; do
    redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends $i
done

pkgs_z="
    binutils
    llvm-dev
    libavutil-dev
    libavcodec-dev
    libavformat-dev
    libavfilter-dev
    libx264-dev
"

for i in $pkgs_z ; do
    redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends $i
done

redirect_cmd apt install -y --force-yes g++
redirect_cmd apt --fix-broken install -y --force-yes


echo ""
echo ""
echo "--------------------------------"
echo "clang version:"
c++ --version
echo "--------------------------------"
echo "--------------------------------"
echo "bazel version:"
bazel --version
echo "--------------------------------"
echo ""
echo ""

cd /c-toxcore

rm -Rf /workspace/_build/
rm -Rf /workspace/auto_tests/
rm -Rf /workspace/cmake/
rm -Rf /workspace/c-toxcore
rm -Rf /workspace/toktok-stack
rm -f  /workspace/CMakeLists.txt

echo "make a local copy ..."
mkdir -p /workspace/c-toxcore/
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/c-toxcore/

# --------- install bazel ---------
wget https://github.com/bazelbuild/bazel/releases/download/2.2.0/bazel_2.2.0-linux-x86_64.deb -O /tmp/b.deb
dpkg -i /tmp/b.deb
# --------- install bazel ---------


# --------- setup bazel ---------
echo "build --jobs=50 --curses=no --verbose_failures" | tee ~/.bazelrc
echo "build --config=linux" | tee -a ~/.bazelrc
echo "build --config=clang" | tee -a ~/.bazelrc
# --------- setup bazel ---------

# --------- fixup clang as default ---------
update-alternatives --remove-all gcc
update-alternatives --remove-all g++
update-alternatives --remove-all cc
update-alternatives --remove-all cc++

rm /usr/bin/gcc
rm /usr/bin/g++
rm /usr/bin/gcc*
rm /usr/bin/c++*
rm /usr/bin/*gcc*

apt-get install -y --force-yes clang-9
# --------- fixup clang as default ---------


# --------- setup toktok-stack ---------
cd /workspace/
git clone --branch=upgrade-bazel --depth=1 https://github.com/iphydf/toktok-stack ./toktok-stack
cd ./toktok-stack/
rm -f ./c-toxcore/
rmdir ./c-toxcore/
ln -s /workspace/c-toxcore/ ./
# --------- setup toktok-stack ---------


# --------- build c-toxcore ---------
cd /workspace/toktok-stack/

# CC=clang bazel build //c-toxcore/...
CC=clang bazel test --copt=-DUSE_IPV6=0 -c opt -k //c-toxcore/...
# --------- build c-toxcore ---------


mkdir -p /artefacts/tsan/
chmod a+rwx -R /workspace/
chmod a+rwx -R /artefacts/

' > $_HOME_/script/do_it___external.sh

chmod a+rx $_HOME_/script/do_it___external.sh


system_to_build_for="ubuntu:18.04"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/bash /script/do_it___external.sh


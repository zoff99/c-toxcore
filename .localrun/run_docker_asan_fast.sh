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

### ** ### cat aa.log |grep --line-buffered -v ' DEBUG '|grep --line-buffered -v 'udp_handle_packet' |grep --line-buffered -v 'network.c:'|more

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

echo "make a local copy ..."
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/

cd /workspace/

echo "#########################"
echo "           IP"
echo "#########################"
ip addr
echo "#########################"
echo "#########################"

CC=clang .circleci/cmake-asan

mkdir -p /artefacts/asan/
chmod a+rwx -R /workspace/
chmod a+rwx -R /artefacts/

cp /workspace/_build/Testing/Temporary/* /artefacts/asan/
cp /workspace/_build/unit_* /workspace/_build/auto_* /artefacts/asan/

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
  /bin/sh -c "apk add bash >/dev/null 2>/dev/null; /bin/bash /script/do_it___external.sh"

# bash /script/do_it___external.sh 2>&1 |grep -C3 -A3 'error: '

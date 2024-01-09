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

# ---- TZ ----
export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/America/New_York /etc/localtime
redirect_cmd apt-get install $qqq -y --force-yes tzdata
redirect_cmd dpkg-reconfigure --frontend noninteractive tzdata
# ---- TZ ----

pkgs="
    ca-certificates
    expect
    ipxe-qemu
    netcat
    qemu-system-s390x
    qemu-utils
    screen
    seabios
    vgabios
    wget
"

for i in $pkgs ; do
    redirect_cmd apt-get install $qqq -y --force-yes --no-install-recommends $i
done

cd /c-toxcore

rm -Rf /workspace/_build/
rm -Rf /workspace/auto_tests/
rm -Rf /workspace/cmake/
rm -f  /workspace/CMakeLists.txt
rm -Rfv /workspace/custom_tests/

echo "make a local copy ..."
redirect_cmd rsync -avz --exclude=".localrun" ./ /workspace/

cd /workspace/

wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/s390x/netboot/vmlinuz-lts -O vmlinuz-lts
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/s390x/netboot/initramfs-lts -O initramfs-lts

rm -f hda.qcow
qemu-img create -f qcow2 hda.qcow 8G



echo "set timeout -1
set alpine_repo \"https://dl-cdn.alpinelinux.org/alpine/v3.19/main\"
set modloop \"https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/s390x/netboot/modloop-lts\"
set ssh_key \"https://raw.githubusercontent.com/iphydf/dockerfiles/s390x/alpine-s390x/src/id_rsa.pub\"

spawn qemu-system-s390x \
  -nographic \
  -m 2048 \
  -net user \
  -net nic \
  -hda hda.qcow \
  -kernel vmlinuz-lts \
  -initrd initramfs-lts \
  -append \"nokaslr alpine_repo=\$alpine_repo modloop=\$modloop ssh_key=\$ssh_key\"

expect \"localhost login:\"
send -- \"root\r\"

expect \"localhost:~#\"
send -- \"echo https://dl-cdn.alpinelinux.org/alpine/v3.19/community >> /etc/apk/repositories\r\"

expect \"localhost:~#\"
send -- \"apk add bash\r\"

expect \"localhost:~#\"
send -- \"/bin/bash\r\"

expect \"localhost:~#\"
send -- \"apk add git autoconf automake libtool make pkgconf gcc ffmpeg-dev x264-dev opus-dev libvpx-dev libsodium-dev linux-headers musl-dev\r\"

expect \"localhost:~#\"
send -- \"export PS1=12345x:\r\"

expect \"12345x:\"
send -- \"git clone https://github.com/zoff99/c-toxcore\r\"

expect \"12345x:\"
send -- \"cd c-toxcore\r\"

# expect \"12345x:\"
# send -- \"autoreconf -vif\r\"

expect \"12345x:\"
send -- \"./configure\r\"

expect \"12345x:\"
send -- \"make\r\"

expect \"12345x:\"
send -- \"sleep 40\r\"
#send -- \"\\001c\"

sleep 40

#expect \"(qemu)\"
#send -- \"savevm login\r\"

#expect \"(qemu)\"
#exit 0

" > /workspace/s390x.expect
script -c "TERM=screen screen expect -f /workspace/s390x.expect"


' > $_HOME_/script/do_it___external.sh

chmod a+rx $_HOME_/script/do_it___external.sh


system_to_build_for="ubuntu:22.04"

cd $_HOME_/
docker run -ti --rm \
  -v $_HOME_/artefacts:/artefacts \
  -v $_HOME_/script:/script \
  -v $_HOME_/../:/c-toxcore \
  -v $_HOME_/workspace:/workspace \
  -e DISPLAY=$DISPLAY \
  "$system_to_build_for" \
  /bin/sh -c "apk add bash >/dev/null 2>/dev/null; /bin/bash /script/do_it___external.sh"


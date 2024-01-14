#! /bin/bash

arch_list="
arm64
s390x
riscv64
mips
"

_HOME2_=$(dirname $0)
export _HOME2_
_HOME_=$(cd $_HOME2_;pwd)
export _HOME_

basedir="$_HOME_""/../"
cd "$basedir"

template='.github/workflows/ci_cross_generic.template'

for i in $arch_list ; do
    echo "generating: ""$i"
    script='.github/workflows/ci_cross_'"$i"'.yml'
    cat "$template" | sed -e 's#@@ARCH@@#'"$i"'#g' > "$script"
    mv "$script" "$script""_"
    j="$i"
    if [ "$i""x" == "arm64x" ] ; then
        j='aarch64'
    fi
    cat "$script""_" | sed -e 's#@@SYSARCH@@#'"$j"'#g' > "$script"
    rm -f "$script""_"
done
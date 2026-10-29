#!/bin/bash
# Build the simulator in WSL from the Windows checkout, as CI does.
# Syncs ~/rb/src to the checkout's HEAD plus its uncommitted changes (a
# clone, because the Windows tree has CRLF line endings), then builds and
# installs into ~/rb/sim. From Windows: wsl -d Ubuntu -- bash <this file>
set -e
WIN=$(cd "$(dirname "$0")/../.." && pwd)
SRC=~/rb/src
SIM=~/rb/sim
[ -d $SRC/.git ] || git clone -q "$WIN" $SRC
cd $SRC
git fetch -q "$WIN" HEAD
git reset -q --hard FETCH_HEAD
git clean -qfd
git -C "$WIN" -c core.autocrlf=true -c core.filemode=false \
  diff HEAD --binary > /tmp/wip.diff
[ -s /tmp/wip.diff ] && git apply --whitespace=nowarn /tmp/wip.diff
if [ ! -f $SIM/Makefile ]; then
  mkdir -p $SIM
  (cd $SIM && $SRC/tools/configure --target=hibyr1 --type=S >/dev/null)
  # GCC 14+ makes these errors; CI's Ubuntu 24.04 GCC only warns.
  sed -i '/^export GCCOPTS=/s/$/ -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion/' $SIM/Makefile
fi
make -C $SIM -j$(nproc) >/tmp/sim-build.log 2>&1 || { tail -30 /tmp/sim-build.log; exit 1; }
make -C $SIM install >/dev/null
[ -d $SIM/simdisk/Music ] || sh tools/simctl/fixtures.sh $SIM/simdisk
echo "sim built: $(git log --oneline -1)"

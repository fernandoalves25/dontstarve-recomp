#!/bin/bash
# Cross-build box64 for the R36S (RK3326) with an old-glibc sysroot, from a box64 checkout:
#   git clone --depth 1 --branch v0.3.7 https://github.com/ptitSeb/box64 && cd box64
#   SYSROOT=/path/to/sysroot-arm64 bash /path/to/build_box64.sh   -> build-r36s/box64
set -e
cd "${BOX64_SRC:-$PWD}"
SR=${SYSROOT:?set SYSROOT to an aarch64 sysroot with glibc <= 2.30 (see scripts/make_sysroot_bionic.py)}
CC=aarch64-linux-gnu-gcc
INC="-nostdinc -isystem $($CC -print-file-name=include) -isystem $SR/usr/include/aarch64-linux-gnu -isystem $SR/usr/include"
LNK="--sysroot=$SR -B$SR/usr/lib/aarch64-linux-gnu -L$SR/lib/aarch64-linux-gnu -L$SR/usr/lib/aarch64-linux-gnu -Wl,-rpath-link=$SR/lib/aarch64-linux-gnu:$SR/usr/lib/aarch64-linux-gnu"
command -v cmake >/dev/null || { echo "no cmake"; exit 1; }
rm -rf build-r36s && mkdir build-r36s && cd build-r36s
cmake .. -DRK3326=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=$CC -DCMAKE_ASM_COMPILER=$CC -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_FLAGS="$INC" -DCMAKE_ASM_FLAGS="$INC" \
  -DCMAKE_EXE_LINKER_FLAGS="$LNK" -DCMAKE_SHARED_LINKER_FLAGS="$LNK" > cmake.log 2>&1 || { tail -n 20 cmake.log; exit 1; }
make -j10 box64 > make.log 2>&1 || { grep -m5 -B2 -A5 "error" make.log; exit 1; }
ls -la box64; file box64 | cut -c1-120
aarch64-linux-gnu-objdump -T box64 | grep -oE "GLIBC_[0-9.]+" | sort -Vu | tail -n 1

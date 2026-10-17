#!/bin/bash
# Compile host_linux.c and link it with the guest objects into a host executable.
# Requires build/obj/*.o from build_guest_obj.sh. Host (x86-64) build for now.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/obj
echo "=== compile host_linux.c ==="
gcc -c -O0 -w -fwrapv -fno-strict-aliasing -fexceptions \
    -I pcrecomp/runtime/recomp32 -I src/recomp/gen \
    src/runtime/host_linux.c -o build/obj/host_linux.o 2>&1 | head -40
[ -f build/obj/host_linux.o ] || { echo "host compile FAILED"; exit 1; }
echo "=== link $(ls build/obj/*.o | wc -l) objects ==="
gcc -o build/dontstarve_host build/obj/*.o -lm > build/link.log 2>&1
rc=$?
head -60 build/link.log
echo "link exit: $rc"
[ $rc -eq 0 ] && ls -l build/dontstarve_host && echo "LINK OK"
exit $rc

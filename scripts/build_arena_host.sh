#!/bin/bash
# Build the 64-bit arena-model host: compile ds_host.c + ds_kernel32.c and link
# with the guest objects (build/obj/recomp_*.o). Requires build_guest_obj.sh first.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/obj
HOSTDEFS="-DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_MEM_QUAL= -DRECOMP_RETADDR=0u"
HOSTINC="-I pcrecomp/runtime/recomp32 -I pcrecomp/runtime/win32hle -I src/runtime"
for f in ds_host ds_kernel32; do
    echo "=== compile $f.c ==="
    gcc -c -O0 -g -w -fwrapv -fno-strict-aliasing -fexceptions $HOSTINC $HOSTDEFS \
        "src/runtime/$f.c" -o "build/obj/$f.o" 2>&1 | head -40
    [ -f "build/obj/$f.o" ] || { echo "$f COMPILE FAILED"; exit 1; }
done
echo "=== link arena host ==="
# *recomp_*.o matches the exe (recomp_*.o) and the lifted CRT DLLs
# (msvcr90_recomp_*.o, msvcp90_recomp_*.o) once they are built.
gcc -g -rdynamic -o build/dontstarve_host \
    build/obj/*recomp_*.o build/obj/ds_host.o build/obj/ds_kernel32.o \
    -lm -lpthread > build/link.log 2>&1
rc=$?; head -40 build/link.log; echo "link exit: $rc"
[ $rc -eq 0 ] && ls -l build/dontstarve_host && echo LINK_OK
exit $rc

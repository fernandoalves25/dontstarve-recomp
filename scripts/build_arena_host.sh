#!/bin/bash
# Build the 64-bit arena-model host: compile ds_host.c + the shim modules and link
# with the guest objects (build/obj/recomp_*.o). Requires build_guest_obj.sh first.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/obj
HOSTDEFS="-DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_HOST_SETJMP -DRECOMP_MEM_QUAL="
# COVERAGE=1 must match the guest build: records executed functions.
[ "${COVERAGE:-0}" = 1 ] && HOSTDEFS="$HOSTDEFS -DRECOMP_TRACE"
HOSTINC="-I pcrecomp/runtime/recomp32 -I pcrecomp/runtime/win32hle -I src/runtime"
# SDL2 backend (DS_VIDEO=sdl at run time) when SDL2 is installed
SDLLIBS=""
if command -v sdl2-config >/dev/null 2>&1; then
    HOSTDEFS="$HOSTDEFS -DDS_SDL $(sdl2-config --cflags)"; SDLLIBS="$(sdl2-config --libs)"
fi
for f in ds_host ds_kernel32 ds_win32 ds_user32 ds_file ds_net ds_thread ds_gles ds_input ds_fmod ds_sdl; do
    echo "=== compile $f.c ==="
    rm -f "build/obj/$f.o"   # a stale object must not hide a compile error
    gcc -c -O0 -g -w -fwrapv -fno-strict-aliasing -fexceptions $HOSTINC $HOSTDEFS \
        "src/runtime/$f.c" -o "build/obj/$f.o" 2>&1 | head -40
    [ -f "build/obj/$f.o" ] || { echo "$f COMPILE FAILED"; exit 1; }
done
echo "=== link arena host ==="
# *recomp_*.o matches the exe (recomp_*.o) and the lifted CRT DLLs
# (msvcr90_recomp_*.o, msvcp90_recomp_*.o) once they are built.
gcc -g -rdynamic -o build/dontstarve_host \
    build/obj/*recomp_*.o build/obj/ds_host.o build/obj/ds_kernel32.o build/obj/ds_win32.o build/obj/ds_user32.o build/obj/ds_file.o build/obj/ds_net.o build/obj/ds_thread.o build/obj/ds_gles.o build/obj/ds_input.o build/obj/ds_fmod.o build/obj/ds_sdl.o \
    $SDLLIBS -lEGL -lGLESv2 -lm -lpthread > build/link.log 2>&1
rc=$?; head -40 build/link.log; echo "link exit: $rc"
[ $rc -eq 0 ] && ls -l build/dontstarve_host && echo LINK_OK
exit $rc

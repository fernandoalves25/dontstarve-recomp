#!/bin/bash
# Build the 64-bit arena-model host: compile ds_host.c + the shim modules and link
# with the guest objects (build/obj/recomp_*.o). Requires build_guest_obj.sh first.
set -u
cd "$(dirname "$0")/.."
# OBJ / OPT / OUT: the same variants as build_guest_obj.sh, e.g.
#   OBJ=build/obj-rel OPT="-O2 -g" OUT=build/dontstarve_host_rel bash scripts/build_arena_host.sh
OBJ=${OBJ:-build/obj}; OPT=${OPT:--O0 -g}; OUT=${OUT:-build/dontstarve_host}
mkdir -p "$OBJ"
HOSTDEFS="-DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_HOST_SETJMP -DRECOMP_X87_WINDOW -DRECOMP_X87_LOCAL -DRECOMP_MEM_BASE=0x100000000ull -DRECOMP_MEM_QUAL="
# COVERAGE=1 must match the guest build: records executed functions.
[ "${COVERAGE:-0}" = 1 ] && HOSTDEFS="$HOSTDEFS -DRECOMP_TRACE"
HOSTINC="-I pcrecomp/runtime/recomp32 -I pcrecomp/runtime/win32hle -I src/runtime"
# SDL2 backend (DS_VIDEO=sdl at run time) when SDL2 is installed
SDLLIBS=""
if command -v sdl2-config >/dev/null 2>&1; then
    HOSTDEFS="$HOSTDEFS -DDS_SDL $(sdl2-config --cflags)"; SDLLIBS="$(sdl2-config --libs)"
fi
for f in ds_host ds_kernel32 ds_win32 ds_user32 ds_file ds_net ds_thread ds_gles ds_input ds_fmod ds_sdl ds_audio; do
    echo "=== compile $f.c ==="
    rm -f "$OBJ/$f.o"   # a stale object must not hide a compile error
    gcc -c $OPT -w -fwrapv -fno-strict-aliasing -fexceptions $HOSTINC $HOSTDEFS \
        "src/runtime/$f.c" -o "$OBJ/$f.o" 2>&1 | head -40
    [ -f "$OBJ/$f.o" ] || { echo "$f COMPILE FAILED"; exit 1; }
done
echo "=== link arena host ==="
# *recomp_*.o matches the exe (recomp_*.o) and the lifted CRT DLLs
# (msvcr90_recomp_*.o, msvcp90_recomp_*.o) once they are built.
gcc -g -rdynamic -o "$OUT" \
    "$OBJ"/*recomp_*.o "$OBJ"/ds_host.o "$OBJ"/ds_kernel32.o "$OBJ"/ds_win32.o "$OBJ"/ds_user32.o "$OBJ"/ds_file.o "$OBJ"/ds_net.o "$OBJ"/ds_thread.o "$OBJ"/ds_gles.o "$OBJ"/ds_input.o "$OBJ"/ds_fmod.o "$OBJ"/ds_sdl.o "$OBJ"/ds_audio.o \
    $SDLLIBS -lvorbisenc -lvorbis -logg -lz -lEGL -lGLESv2 -lm -lpthread > build/link.log 2>&1
rc=$?; head -40 build/link.log; echo "link exit: $rc"
[ $rc -eq 0 ] && ls -l "$OUT" && echo LINK_OK
exit $rc

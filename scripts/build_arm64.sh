#!/bin/bash
# Cross-build the R36S (aarch64) host: every lifted guest file plus the runtime,
# at -O2, with aarch64-linux-gnu-gcc. Objects land in build/obj-arm64.
#   bash scripts/build_arm64.sh            # compile only (no aarch64 libs needed)
#   SYSROOT=/path bash scripts/build_arm64.sh link
#       also link build/dontstarve_host.aarch64 against an aarch64 sysroot that
#       has SDL2, EGL and GLESv2 (the device's, or a PortMaster build image).
#       SYSROOT=/ uses Debian/Ubuntu multiarch arm64 packages instead
#       (libsdl2-dev:arm64 libegl-dev:arm64 libgles-dev:arm64); the result runs
#       under qemu-aarch64-static -L /usr/aarch64-linux-gnu.
# The ArkOS image is glibc 2.30: build in an old-enough sysroot/container for
# a binary that loads there (see docs/01-ARCHITECTURE.md, Toolchain).
set -u
cd "$(dirname "$0")/.."
CC=${CC:-aarch64-linux-gnu-gcc}
OBJ=build/obj-arm64
mkdir -p "$OBJ"
DEFS="-DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_HOST_SETJMP -DRECOMP_MEM_QUAL="
FLAGS="-O2 -w -fwrapv -fno-strict-aliasing -fexceptions"
export CC OBJ DEFS FLAGS
: > build/arm64_errors.log
echo "guest start: $(date +%T)"
# biggest files first, so the long ones do not finish last on one core
ls -S src/recomp/gen/*.c | xargs -P "${JOBS:-4}" -I FILE sh -c \
  'o="$OBJ/$(basename "FILE" .c).o"; [ "$o" -nt "FILE" ] && exit 0;
   $CC -c $FLAGS -I pcrecomp/runtime/recomp32 -I src/recomp/gen $DEFS "FILE" -o "$o" 2>>build/arm64_errors.log \
   || echo "FAIL: FILE" >>build/arm64_errors.log'
echo "guest done: $(date +%T)  objects: $(ls "$OBJ"/*recomp_*.o 2>/dev/null | wc -l) / $(ls src/recomp/gen/*.c | wc -l)"
SDLDEF=""; LIBDIR=""
if [ "${SYSROOT:-}" = / ]; then SDLDEF="-DDS_SDL -I/usr/include/SDL2 -D_REENTRANT"; LIBDIR="-L/usr/lib/aarch64-linux-gnu"
elif [ -n "${SYSROOT:-}" ]; then SDLDEF="-DDS_SDL --sysroot=$SYSROOT -I$SYSROOT/usr/include/SDL2 -D_REENTRANT"; LIBDIR="--sysroot=$SYSROOT"; fi
for f in ds_host ds_kernel32 ds_win32 ds_user32 ds_file ds_net ds_thread ds_gles ds_input ds_fmod ds_sdl; do
    $CC -c $FLAGS -g $SDLDEF -I pcrecomp/runtime/recomp32 -I pcrecomp/runtime/win32hle -I src/runtime \
        $DEFS "src/runtime/$f.c" -o "$OBJ/$f.o" 2>>build/arm64_errors.log \
        || echo "FAIL: $f.c" >>build/arm64_errors.log
done
echo "errors: $(grep -c FAIL build/arm64_errors.log)"
if [ "${1:-}" = link ]; then
    $CC $LIBDIR -rdynamic -o build/dontstarve_host.aarch64 "$OBJ"/*.o \
        -lSDL2 -lEGL -lGLESv2 -lm -lpthread && echo "LINK_OK build/dontstarve_host.aarch64"
fi

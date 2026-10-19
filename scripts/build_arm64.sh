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
# The ArkOS image is glibc 2.30: link against an old sysroot for a binary that
# loads there -- scripts/make_sysroot_bionic.py builds one (glibc 2.27):
#   SYSROOT=$PWD/build/sysroot-arm64 bash scripts/build_arm64.sh link
set -u
cd "$(dirname "$0")/.."
CC=${CC:-aarch64-linux-gnu-gcc}
OBJ=build/obj-arm64
mkdir -p "$OBJ"
DEFS="-DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_HOST_SETJMP -DRECOMP_X87_WINDOW -DRECOMP_X87_LOCAL -DRECOMP_MEM_BASE=0x100000000ull -DRECOMP_MEM_QUAL="
# -O2 plus the loop/inlining passes -fprofile-use would switch on: on the
# lifted code (goto-heavy, many small calls) they cut the game's CPU time per
# frame by ~29% on x86 (scripts/bench.sh: 6.3 -> 4.5 ms); a real profile on
# top of them added nothing measurable.
FAST="-funroll-loops -fpeel-loops -ftracer -finline-functions -fipa-cp-clone -funswitch-loops -fsplit-loops -fpredictive-commoning -fgcse-after-reload -ftree-loop-distribute-patterns"
FLAGS="-O2 $FAST -mcpu=cortex-a35 -w -fwrapv -fno-strict-aliasing -fexceptions"   # RK3326 = Cortex-A35
# PGO=dir: profile-guided build from a training run's .gcda files, renamed for
# this object directory (#...#build#obj-arm64#<file>.gcda; see docs/PERFORMANCE.md)
[ -n "${PGO:-}" ] && FLAGS="$FLAGS -fprofile-use=$PGO -fprofile-partial-training -fprofile-correction"
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
elif [ -n "${SYSROOT:-}" ]; then
    # Ubuntu's cross gcc searches /usr/aarch64-linux-gnu whatever --sysroot says:
    # force the sysroot's headers, crt files and libraries (its glibc) first.
    MA="aarch64-linux-gnu"
    SDLDEF="-DDS_SDL -nostdinc -isystem $($CC -print-file-name=include) -isystem $SYSROOT/usr/include/$MA \
            -isystem $SYSROOT/usr/include -I$SYSROOT/usr/include/SDL2 -D_REENTRANT -DMESA_EGL_NO_X11_HEADERS -DEGL_NO_X11"
    LIBDIR="--sysroot=$SYSROOT -B$SYSROOT/usr/lib/$MA -L$SYSROOT/lib/$MA -L$SYSROOT/usr/lib/$MA \
            -Wl,-rpath-link=$SYSROOT/lib/$MA:$SYSROOT/usr/lib/$MA"
fi
for f in ds_host ds_kernel32 ds_win32 ds_user32 ds_file ds_net ds_thread ds_gles ds_input ds_fmod ds_audio ds_sdl ds_sample; do
    $CC -c $FLAGS -g $SDLDEF -I pcrecomp/runtime/recomp32 -I pcrecomp/runtime/win32hle -I src/runtime \
        $DEFS "src/runtime/$f.c" -o "$OBJ/$f.o" 2>>build/arm64_errors.log \
        || echo "FAIL: $f.c" >>build/arm64_errors.log
done
echo "errors: $(grep -c FAIL build/arm64_errors.log)"
if [ "${1:-}" = link ]; then
    # the libraries' own dependencies (ALSA, X11, ...) are the device's business
    $CC $LIBDIR -rdynamic -Wl,--allow-shlib-undefined -o build/dontstarve_host.aarch64 "$OBJ"/*.o \
        -Wl,-Bstatic -lvorbisenc -lvorbis -logg -lz -Wl,-Bdynamic \
        -lSDL2 -lEGL -lGLESv2 -lm -lpthread -ldl && echo "LINK_OK build/dontstarve_host.aarch64"
    echo "newest glibc symbol needed: $(aarch64-linux-gnu-objdump -T build/dontstarve_host.aarch64 \
        | grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n -u | tail -1)"
fi

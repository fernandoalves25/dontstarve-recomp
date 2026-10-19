#!/bin/bash
# Compile all lifted guest C to object files (host compile-check / link input).
# Objects land in build/obj (gitignored, persistent on E:). Run via WSL.
set -u
cd "$(dirname "$0")/.."
# OBJ / OPT pick the variant: the default -O0 build in build/obj, or e.g.
#   OBJ=build/obj-rel OPT="-O2 -g" bash scripts/build_guest_obj.sh   (profiling)
OBJ=${OBJ:-build/obj}
OPT=${OPT:--O0}
# COVERAGE=1: record which lifted functions actually execute (-DRECOMP_TRACE).
# Must match the host build (build_arena_host.sh honours the same variable).
COVDEF=""; [ "${COVERAGE:-0}" = 1 ] && COVDEF="-DRECOMP_TRACE"
# EXTRADEFS=-DRECOMP_NO_ICALL_TRACE for a release build (no indirect-call ring for crash reports)
export COVDEF OBJ OPT EXTRADEFS
mkdir -p "$OBJ"
: > build/compile_errors.log
echo "start: $(date +%T)"
ls -S src/recomp/gen/*.c | xargs -P "${JOBS:-4}" -I FILE sh -c \
  'gcc -c $OPT -w -fwrapv -fno-strict-aliasing -fexceptions \
   -I pcrecomp/runtime/recomp32 -I src/recomp/gen \
   -DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_HOST_SETJMP -DRECOMP_X87_WINDOW -DRECOMP_X87_LOCAL -DRECOMP_MEM_BASE=0x100000000ull -DRECOMP_MEM_QUAL= -DRECOMP_LCACHE_BITS=16 -DRECOMP_SETJMP_VA0=0x785926F0u -DRECOMP_SETJMP_VA1=0x785926B0u $COVDEF ${EXTRADEFS:-} \
   "FILE" -o "$OBJ/$(basename "FILE" .c).o" 2>>build/compile_errors.log \
   || echo "FAIL: FILE" >>build/compile_errors.log'
echo "done: $(date +%T)"
echo "objects: $(ls "$OBJ"/*.o 2>/dev/null | wc -l) / $(ls src/recomp/gen/*.c | wc -l)"
echo "compile error lines: $(wc -l < build/compile_errors.log)"
# Undefined-symbol surface (what the runtime host must provide, minus libc)
nm "$OBJ"/*.o 2>/dev/null | grep ' U ' | sed 's/.* U //' | sort -u > build/undef_all.txt
echo "distinct undefined symbols: $(wc -l < build/undef_all.txt)"

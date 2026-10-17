#!/bin/bash
# Compile all lifted guest C to object files (host compile-check / link input).
# Objects land in build/obj (gitignored, persistent on E:). Run via WSL.
set -u
cd "$(dirname "$0")/.."
OBJ=build/obj
mkdir -p "$OBJ"
: > build/compile_errors.log
echo "start: $(date +%T)"
ls src/recomp/gen/*.c | xargs -P 8 -I FILE sh -c \
  'gcc -c -O0 -w -fwrapv -fno-strict-aliasing -fexceptions \
   -I pcrecomp/runtime/recomp32 -I src/recomp/gen \
   -DRECOMP_STRICT_ICALL -DRECOMP_LOCAL_REGS -DRECOMP_X87_WINDOW -DRECOMP_MEM_QUAL= \
   "FILE" -o "build/obj/$(basename "FILE" .c).o" 2>>build/compile_errors.log \
   || echo "FAIL: FILE" >>build/compile_errors.log'
echo "done: $(date +%T)"
echo "objects: $(ls "$OBJ"/*.o 2>/dev/null | wc -l) / $(ls src/recomp/gen/*.c | wc -l)"
echo "compile error lines: $(wc -l < build/compile_errors.log)"
# Undefined-symbol surface (what the runtime host must provide, minus libc)
nm "$OBJ"/*.o 2>/dev/null | grep ' U ' | sed 's/.* U //' | sort -u > build/undef_all.txt
echo "distinct undefined symbols: $(wc -l < build/undef_all.txt)"

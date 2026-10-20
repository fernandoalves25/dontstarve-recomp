#!/bin/bash
# Assemble the PortMaster package: build/port/dontstarve-recomp.zip with
#   dontstarve-recomp.sh               the launcher (port/dontstarve-recomp.sh)
#   dontstarve-recomp/dontstarve_host.aarch64 the aarch64 build (scripts/build_arm64.sh link)
#   dontstarve-recomp/tools/audio/            fsb5.py + import_setups.py (low-rate Vorbis setups)
#   dontstarve-recomp/port.json, README.md    PortMaster metadata and what to copy where
#   dontstarve-recomp/gamedata/mods/r36s-hud  a small mod: the HUD 1.4x larger (port/mods/)
# No game material goes in: the player copies their own DRM-free install
# (bin/dontstarve.exe, data/, mods/) to dontstarve-recomp/gamedata/ and the VC9 x86
# runtime DLLs to dontstarve-recomp/crt/ -- the README in the zip says how.
set -eu
cd "$(dirname "$0")/.."
BIN=build/dontstarve_host.aarch64
[ -f "$BIN" ] || { echo "no $BIN: run SYSROOT=\$PWD/build/sysroot-arm64 bash scripts/build_arm64.sh link"; exit 1; }
OUT=build/port; rm -rf "$OUT"; mkdir -p "$OUT/dontstarve-recomp/tools/audio" "$OUT/dontstarve-recomp/gamedata" "$OUT/dontstarve-recomp/crt"
cp port/dontstarve-recomp.sh "$OUT/"
cp "$BIN" "$OUT/dontstarve-recomp/"
aarch64-linux-gnu-strip "$OUT/dontstarve-recomp/dontstarve_host.aarch64" 2>/dev/null || true
cp tools/audio/fsb5.py tools/audio/import_setups.py "$OUT/dontstarve-recomp/tools/audio/"
cp port/port.json port/README.md "$OUT/dontstarve-recomp/"
mkdir -p "$OUT/dontstarve-recomp/gamedata/mods" && cp -r port/mods/r36s-hud "$OUT/dontstarve-recomp/gamedata/mods/"   # bigger HUD
( cd "$OUT" && rm -f dontstarve-recomp.zip && zip -qr dontstarve-recomp.zip dontstarve-recomp.sh dontstarve-recomp )
ls -la "$OUT/dontstarve-recomp.zip"

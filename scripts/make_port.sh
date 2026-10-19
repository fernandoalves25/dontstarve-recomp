#!/bin/bash
# Assemble the PortMaster package: build/port/dontstarve-recomp.zip with
#   dontstarve.sh                      the launcher (port/dontstarve.sh)
#   dontstarve/dontstarve_host.aarch64 the aarch64 build (scripts/build_arm64.sh link)
#   dontstarve/tools/audio/            fsb5.py + import_setups.py (low-rate Vorbis setups)
#   dontstarve/port.json, README.md    PortMaster metadata and what to copy where
#   dontstarve/gamedata/mods/r36s-hud  a small mod: the HUD 1.4x larger (port/mods/)
#   dontstarve/home/vorbis_setups.bin  the Vorbis setup headers, if built here
# No game material goes in: the player copies their own DRM-free install
# (bin/dontstarve.exe, data/, mods/) to dontstarve/gamedata/ and the VC9 x86
# runtime DLLs to dontstarve/crt/ -- the README in the zip says how.
set -eu
cd "$(dirname "$0")/.."
BIN=build/dontstarve_host.aarch64
[ -f "$BIN" ] || { echo "no $BIN: run SYSROOT=\$PWD/build/sysroot-arm64 bash scripts/build_arm64.sh link"; exit 1; }
OUT=build/port; rm -rf "$OUT"; mkdir -p "$OUT/dontstarve/tools/audio" "$OUT/dontstarve/gamedata" "$OUT/dontstarve/crt"
cp port/dontstarve.sh "$OUT/"
cp "$BIN" "$OUT/dontstarve/"
aarch64-linux-gnu-strip "$OUT/dontstarve/dontstarve_host.aarch64" 2>/dev/null || true
cp tools/audio/fsb5.py tools/audio/import_setups.py "$OUT/dontstarve/tools/audio/"
cp port/port.json port/README.md "$OUT/dontstarve/"
mkdir -p "$OUT/dontstarve/gamedata/mods" && cp -r port/mods/r36s-hud "$OUT/dontstarve/gamedata/mods/"   # bigger HUD
# the Vorbis setup headers (libvorbis output + python-fsb5's MIT table, no game
# material): all sounds play on the first run without python3 or a download
SETUPS=${SETUPS:-build/home/vorbis_setups.bin}
if [ -f "$SETUPS" ]; then mkdir -p "$OUT/dontstarve/home" && cp "$SETUPS" "$OUT/dontstarve/home/" && touch "$OUT/dontstarve/home/.setups_imported"; \
else echo "note: no $SETUPS (run tools/audio/import_setups.py): the low-rate sounds need it on the device"; fi
( cd "$OUT" && rm -f dontstarve-recomp.zip && zip -qr dontstarve-recomp.zip dontstarve.sh dontstarve )
ls -la "$OUT/dontstarve-recomp.zip"

#!/bin/bash
# CPU benchmark of the game code: load a saved world, stand still, skip the
# draws (DS_NO_DRAW) and average the per-frame work (frame time minus the
# frame limiter's wait) over frames 3000..4800. Needs a save in $HOME_SRC
# whose slot 1 Continue is at the menu positions below (640x480).
#   HOME_SRC=build/home-save BIN=build/dontstarve_host_rel bash scripts/bench.sh
set -u
cd "$(dirname "$0")/.."
BIN=${BIN:-build/dontstarve_host_rel}; HOME_SRC=${HOME_SRC:-build/home-save}
H=build/home-bench; rm -rf "$H"; cp -r "$HOME_SRC" "$H"
DS_NO_DRAW=1 DS_HOME_DIR="$H" DS_INPUT="1000 click 540 205; 1600 click 320 175; 2200 click 320 270" \
DS_EXIT_AFTER_FRAMES=4800 timeout 1800 "$BIN" --run-entry 2>&1 | grep -o 'frame [0-9]* .*work)' | \
awk '{f=$2; sub(/.*waiting, /,""); w=$1} f>3000 {s+=w; n++} END {if (n) printf "work: %.2f ms/frame (mean of %d samples of 120 frames)\n", s/n, n; else print "no samples"}'

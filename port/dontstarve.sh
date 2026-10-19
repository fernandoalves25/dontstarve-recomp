#!/bin/bash
# PortMaster launcher for the native (recompiled) Don't Starve on the R36S.
# Layout under ports/dontstarve/:
#   dontstarve_host.aarch64   this repo's build (scripts/build_arm64.sh)
#   gamedata/bin/dontstarve.exe   YOUR DRM-free standalone exe (the lifted code was built from it)
#   gamedata/data/, gamedata/mods/  YOUR game data (copied from your install)
#   crt/msvcr90.dll, crt/msvcp90.dll  the VC9 x86 runtime (9.0.30729.x, from a vcredist)
#   home/  saves, settings.ini, log.txt (created on first run)
#   home/vorbis_setups.bin  the Vorbis setup headers (built on first run; for
#          the low-rate waves run tools/audio/import_setups.py on a PC with
#          DS_HOME_DIR pointing here, or let this script run it if python3 and
#          tools/audio/ are present)
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else controlfolder="/roms/ports/PortMaster"; fi
source "$controlfolder/control.txt"
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/dontstarve"
cd "$GAMEDIR" || exit 1
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

for f in gamedata/bin/dontstarve.exe gamedata/data crt/msvcr90.dll crt/msvcp90.dll; do
    [ -e "$f" ] || { echo "missing ports/dontstarve/$f -- see ports/dontstarve/README.md"; pm_finish; exit 1; }
done

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export DS_VIDEO=sdl                       # SDL2 window (KMSDRM) + GLES2 on the Mali GPU
export DS_SCREEN="${DISPLAY_WIDTH:-640}x${DISPLAY_HEIGHT:-480}"
export DS_GAME_DIR="$GAMEDIR/gamedata"    # Z:\dontstarve\ (bin\, data\, mods\)
export DS_HOME_DIR="$GAMEDIR/home"        # Z:\home\ (Documents\Klei\DoNotStarve)
export DS_CRT_DIR="$GAMEDIR/crt"
export DS_AUDIO=sdl                       # FMOD events through the SDL mixer

# the bigger-HUD mod (gamedata/mods/r36s-hud): enabled once; DS_BIG_HUD=0 leaves it alone
MS=gamedata/mods/modsettings.lua
if [ "${DS_BIG_HUD:-1}" != 0 ] && [ -d gamedata/mods/r36s-hud ] && ! grep -qs 'r36s-hud' "$MS"; then
    echo 'ForceEnableMod("r36s-hud")' >> "$MS"
fi

if [ ! -f "$DS_HOME_DIR/.setups_imported" ] && [ -f tools/audio/import_setups.py ] && command -v python3 >/dev/null; then
    python3 -I tools/audio/import_setups.py && touch "$DS_HOME_DIR/.setups_imported"
fi

# full clocks while playing: CPU, GPU and memory (devfreq) governors to
# "performance", the previous ones back on exit. DS_PERF=0 leaves them alone.
GOVS=()
if [ "${DS_PERF:-1}" != 0 ]; then
    for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/class/devfreq/*/governor; do
        [ -w "$g" ] || [ -n "$ESUDO" ] || continue
        [ -f "$g" ] || continue
        old=$(cat "$g" 2>/dev/null) || continue
        grep -qw performance "$(dirname "$g")/$( [ "${g##*/}" = governor ] && echo available_governors || echo scaling_available_governors)" 2>/dev/null || continue
        echo performance | $ESUDO tee "$g" >/dev/null 2>&1 && GOVS+=("$g=$old") && echo "governor $g: $old -> performance"
    done
fi

# the built-in profiler: every 30 s the log gets CPU per thread, the clocks
# and the hottest functions (DS_SAMPLE=0 turns it off)
export DS_SAMPLE=${DS_SAMPLE:-1} DS_SAMPLE_MS=${DS_SAMPLE_MS:-5} DS_SAMPLE_SEC=${DS_SAMPLE_SEC:-30}

$ESUDO chmod +x ./dontstarve_host.aarch64
# quitting: the game itself ends on Select+Start held for a second; gptokeyb
# (no key mapping, only PortMaster's kill hotkey) is the fallback if it hangs
$ESUDO chmod 666 /dev/uinput 2>/dev/null
[ -n "$GPTOKEYB" ] && $GPTOKEYB "dontstarve_host.aarch64" &
./dontstarve_host.aarch64 --run-entry "$GAMEDIR/gamedata/bin/dontstarve.exe"

for e in "${GOVS[@]}"; do echo "${e#*=}" | $ESUDO tee "${e%%=*}" >/dev/null 2>&1; done

pm_finish

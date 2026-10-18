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

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export DS_VIDEO=sdl                       # SDL2 window (KMSDRM) + GLES2 on the Mali GPU
export DS_SCREEN="${DISPLAY_WIDTH:-640}x${DISPLAY_HEIGHT:-480}"
export DS_GAME_DIR="$GAMEDIR/gamedata"    # Z:\dontstarve\ (bin\, data\, mods\)
export DS_HOME_DIR="$GAMEDIR/home"        # Z:\home\ (Documents\Klei\DoNotStarve)
export DS_CRT_DIR="$GAMEDIR/crt"
export DS_AUDIO=sdl                       # FMOD events through the SDL mixer

if [ ! -f "$DS_HOME_DIR/.setups_imported" ] && [ -f tools/audio/import_setups.py ] && command -v python3 >/dev/null; then
    python3 -I tools/audio/import_setups.py && touch "$DS_HOME_DIR/.setups_imported"
fi

$ESUDO chmod +x ./dontstarve_host.aarch64
./dontstarve_host.aarch64 --run-entry "$GAMEDIR/gamedata/bin/dontstarve.exe"

pm_finish

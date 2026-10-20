#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/dontstarve
CONFDIR="$GAMEDIR/conf"
cd "$GAMEDIR" || exit 1

# STOP_ES=1 in port.cfg: close EmulationStation while the game runs (it holds 200-400 MB)
# and reopen it afterwards. es_release.sh starts this script again outside ES's service.
if [ -z "$ES_RELEASED" ] && grep -qx "STOP_ES=1" "$GAMEDIR/port.cfg"; then
  bash "$GAMEDIR/tools/es_release.sh" --system ports "$0" "$@"
  exit $?
fi

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

source "$GAMEDIR/port.cfg"

# Unpack GOG Linux installers dropped into gamedata/ (base game sorts first)
for installer in "$GAMEDIR"/gamedata/*.sh; do
  [ -f "$installer" ] || continue
  if ! command -v python3 > /dev/null; then
    pm_message "python3 not found. Extract the game on a PC (see LEIA-ME.md)."
    sleep 8
    exit 1
  fi
  pm_message "Extracting $(basename "$installer"), this takes a few minutes..."
  python3 "$GAMEDIR/tools/ds_optimize.py" extract "$installer" --out "$GAMEDIR/gamedata" && rm -f "$installer"
done

# Use the 64-bit Linux build: box64 runs it on every aarch64 CFW
BINARY=""
while read -r f; do
  if [ "$(od -An -tu1 -j4 -N1 "$f" | tr -d ' ')" = "2" ]; then
    BINARY="$f"
    break
  fi
done < <(find "$GAMEDIR/gamedata" -type f -name dontstarve)

if [ -z "$BINARY" ]; then
  if [ -n "$(find "$GAMEDIR/gamedata" -type f -name 'dontstarve_steam*' | head -n 1)" ]; then
    pm_message "The Steam build only starts with the Steam client running. Use the GOG Linux version."
  elif [ -n "$(find "$GAMEDIR/gamedata" -type f -name dontstarve | head -n 1)" ]; then
    pm_message "Only a 32-bit game binary was found. This port needs the 64-bit GOG Linux build."
  else
    pm_message "Game files not found. Put the GOG Linux installers in ports/dontstarve/gamedata/."
  fi
  sleep 8
  exit 1
fi
BINDIR=$(dirname "$BINARY")
ROOTDIR=$(dirname "$BINDIR")
chmod +x "$BINARY" "$GAMEDIR/box64/box64"

# Halve animation and ground textures once (lossless, drops the top mipmap)
if [ "$OPTIMIZE_TEXTURES" = "1" ] && command -v python3 > /dev/null; then
  [ -f "$ROOTDIR/data/.r36s_optimized" ] || pm_message "Optimizing textures (first launch only)..."
  python3 "$GAMEDIR/tools/ds_optimize.py" optimize "$ROOTDIR" --jobs 4
fi

# Performance mod
MODSETTINGS="$ROOTDIR/mods/modsettings.lua"
mkdir -p "$ROOTDIR/mods"
touch "$MODSETTINGS"
sed -i '/ForceEnableMod("r36s_performance")/d' "$MODSETTINGS"
if [ "$PERF_MOD" = "1" ]; then
  cp -rf "$GAMEDIR/mods/r36s_performance" "$ROOTDIR/mods/"
  SHOW=false
  LOGFPS=false
  [ "$SHOW_FPS" = "1" ] && SHOW=true
  [ "$DEBUG_FPS" = "1" ] && LOGFPS=true
  printf -- '-- Written by "Dont Starve.sh" from port.cfg\nreturn { show_fps = %s, log_fps = %s }\n' \
    "$SHOW" "$LOGFPS" > "$ROOTDIR/mods/r36s_performance/r36s_options.lua"
  [ -n "$(tail -c 1 "$MODSETTINGS")" ] && echo >> "$MODSETTINGS"
  echo 'ForceEnableMod("r36s_performance")' >> "$MODSETTINGS"
fi

# The bigger HUD/text mod (port/mods/r36s-hud in this repository), when it was copied to
# dontstarve/mods/: installed and force-enabled like r36s_performance. HUD_MOD=0 skips it.
sed -i '/ForceEnableMod("r36s-hud")/d' "$MODSETTINGS"
if [ "${HUD_MOD:-1}" = "1" ] && [ -d "$GAMEDIR/mods/r36s-hud" ]; then
  cp -rf "$GAMEDIR/mods/r36s-hud" "$ROOTDIR/mods/"
  [ -n "$(tail -c 1 "$MODSETTINGS")" ] && echo >> "$MODSETTINGS"
  echo 'ForceEnableMod("r36s-hud")' >> "$MODSETTINGS"
fi

# Saves and settings live in conf/.klei (the game uses $HOME/.klei/DoNotStarve)
SETTINGS="$CONFDIR/.klei/DoNotStarve/settings.ini"
mkdir -p "$(dirname "$SETTINGS")"
[ -f "$SETTINGS" ] || cp "$GAMEDIR/settings.ini" "$SETTINGS"
# The engine's small-texture mode halves textures at load. Without the optimized files it's
# always on; on top of them only with SMALL_TEXTURES=1 (texture memory is what runs a 1 GB
# device out of RAM once a world loads).
SMALL_TEX=true
[ "$OPTIMIZE_TEXTURES" = "1" ] && [ -f "$ROOTDIR/data/.r36s_optimized" ] && [ "$SMALL_TEXTURES" != "1" ] && SMALL_TEX=false
sed -i -e "s/^fullscreen_width *=.*/fullscreen_width = $DISPLAY_WIDTH/" \
  -e "s/^fullscreen_height *=.*/fullscreen_height = $DISPLAY_HEIGHT/" \
  -e "s/^use_small_textures *=.*/use_small_textures = $SMALL_TEX/" "$SETTINGS"

# Sound: the game's FMOD runs emulated and now and then misses its deadline, so the 85 ms
# buffer it asks ALSA for runs dry and the sound crackles. Give it a bigger buffer through
# dmix (the game reads $HOME/.asoundrc, and HOME is conf/). AUDIO_BUFFER=0 leaves ALSA alone.
# The card runs at FMOD's mixing rate when it supports it (otherwise ALSA picks the nearest and
# converts), so the sound isn't resampled a second time.
DMIX_RATE=48000
[ "$FMOD_TUNE" = "1" ] && [ "${FMOD_RATE:-0}" -gt 0 ] 2>/dev/null && DMIX_RATE=$FMOD_RATE
if [ "${AUDIO_BUFFER:-0}" -gt 0 ] 2>/dev/null; then
  cat > "$CONFDIR/.asoundrc" << EOF
pcm.!default {
  type plug
  slave.pcm "dontstarve_dmix"
}
pcm.dontstarve_dmix {
  type dmix
  ipc_key 5765
  ipc_perm 0666
  slave {
    pcm "hw:0,0"
    rate $DMIX_RATE
    period_size 1024
    buffer_size $AUDIO_BUFFER
  }
}
ctl.!default { type hw card 0 }
EOF
else
  rm -f "$CONFDIR/.asoundrc"
fi

GAME_ENV=(
  HOME="$CONFDIR"
  LIBGL_NOBANNER=1
  LIBGL_NOERROR=1
  BOX64_NORCFILES=1
  BOX64_LD_LIBRARY_PATH="$GAMEDIR/libs.x64:$BINDIR/lib64:$BINDIR"
)
[ "$TEX_16BIT" = "1" ] && GAME_ENV+=(LIBGL_AVOID16BITS=0)
# Small fixes loaded into the game (tools/r36s_hooks): retune FMOD before it starts (mix
# further ahead so a slow frame doesn't leave the sound card empty) and hide SDL's mouse pointer
HOOKS="$GAMEDIR/libs.x64/r36s_hooks.so"
# Full clocks while playing: CPU, GPU and memory (devfreq) governors to "performance", the
# previous ones back on exit. ArkOS boots with the memory at 328 MHz (dmc_ondemand), which
# alone took the game from ~20 to ~12 fps. PERF_GOV=0 in port.cfg leaves them alone.
GOVS=()
if [ "${PERF_GOV:-1}" = "1" ]; then
  for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/class/devfreq/*/governor; do
    [ -f "$g" ] || continue
    old=$(cat "$g" 2>/dev/null) || continue
    echo performance | $ESUDO tee "$g" >/dev/null 2>&1 && GOVS+=("$g=$old")
  done
fi

# The game's simulation (Lua) runs on one thread that sits at 100% of a core; the other
# threads (FMOD's mixer, the GPU driver) run beside it. PIN_MAIN=1 keeps the main thread
# alone on CPU 3 and the game's other threads on CPUs 0-2, so nothing preempts it. A
# small loop reapplies it every 5 s (threads the game creates later inherit CPU 3).
if [ "${PIN_MAIN:-0}" = "1" ] && command -v taskset >/dev/null; then
  (
    for i in $(seq 1 90); do P=$(pgrep -x dontstarve | head -1); [ -n "$P" ] && break; sleep 2; done
    while [ -n "$P" ] && [ -d "/proc/$P" ]; do
      for t in /proc/$P/task/*; do
        tid=${t##*/}; want=0-2; [ "$tid" = "$P" ] && want=3
        [ "$(taskset -pc "$tid" 2>/dev/null | awk -F': ' '{print $2}')" = "$want" ] || taskset -pc "$want" "$tid" >/dev/null 2>&1
      done
      sleep 5
    done
  ) &
fi

if [ -f "$HOOKS" ] && { [ "$FMOD_TUNE" = "1" ] || [ "$HIDE_CURSOR" = "1" ]; }; then
  GAME_ENV+=(BOX64_LD_PRELOAD="$HOOKS" R36S_HIDE_CURSOR="${HIDE_CURSOR:-0}")
  [ "$FMOD_TUNE" = "1" ] && GAME_ENV+=(
    R36S_FMOD_BLOCKS="${FMOD_BLOCKS:-0}"
    R36S_FMOD_RATE="${FMOD_RATE:-0}"
    R36S_FMOD_VOICES="${FMOD_VOICES:-0}"
    R36S_FMOD_NOREVERB="${FMOD_NOREVERB:-0}"
    R36S_FMOD_RESAMPLER="${FMOD_RESAMPLER:-0}"
  )
fi
[ "$BOX64_FAST" = "1" ] && GAME_ENV+=(BOX64_DYNAREC_SAFEFLAGS=0 BOX64_DYNAREC_FORWARD=1024 BOX64_DYNAREC_CALLRET=1)
# Extra box64 settings from port.cfg, space separated (e.g. "BOX64_DYNAREC_BIGBLOCK=3")
[ -n "$BOX64_EXTRA" ] && GAME_ENV+=($BOX64_EXTRA)

cd "$BINDIR" || exit 1
# Older/clone PortMaster builds don't ship pm_platform_helper
command -v pm_platform_helper >/dev/null 2>&1 && pm_platform_helper "$GAMEDIR/box64/box64"

if [ "$RENDER_MODE" = "native" ]; then
  # libgl_*.txt locates gl4es from $PWD, so source it from the port root
  cd "$GAMEDIR" || exit 1
  if [ -f "${controlfolder}/libgl_${CFW_NAME}.txt" ]; then
    source "${controlfolder}/libgl_${CFW_NAME}.txt"
  else
    source "${controlfolder}/libgl_default.txt"
  fi
  cd "$BINDIR" || exit 1
  # gptokeyb needs uinput writable
  $ESUDO chmod 666 /dev/uinput 2>/dev/null
  # The game's bundled SDL2 is from 2013 and can only open a window through X11.
  # Without BOX64_EMULATED_LIBS, box64 swaps it for the system's native ARM SDL2,
  # which draws straight to the display (KMSDRM). GL goes through gl4es.
  GAME_ENV+=(
    SDL_VIDEO_GL_DRIVER="$GAMEDIR/gl4es.aarch64/libGL.so.1"
    SDL_VIDEO_EGL_DRIVER="$GAMEDIR/gl4es.aarch64/libEGL.so.1"
    BOX64_LIBGL="$GAMEDIR/gl4es.aarch64/libGL.so.1"
  )
  # The game expects the Xbox 360 layout, but this pad reports its d-pad and L2/R2 as plain
  # buttons. gptokeyb2 -x mirrors it as a virtual Xbox 360 pad (and handles the exit hotkey);
  # once that pad shows up, hide the real one from the game so inputs don't double up.
  $GPTOKEYB2 "dontstarve" -x &
  pad_ids() {
    for d in /sys/class/input/js*/device/id; do
      [ -d "$d" ] && echo "$(cat "$d/vendor"):$(cat "$d/product")"
    done
  }
  for _ in $(seq 10); do
    pad_ids | grep -qx "045e:028e" && break
    sleep 0.5
  done
  if pad_ids | grep -qx "045e:028e"; then
    PAD_IGNORE=$(pad_ids | grep -vx "045e:028e" | sed 's|^\(.*\):\(.*\)$|0x\1/0x\2|' | paste -sd,)
    [ -n "$PAD_IGNORE" ] && GAME_ENV+=(SDL_GAMECONTROLLER_IGNORE_DEVICES="$PAD_IGNORE")
  else
    echo "[R36S] virtual Xbox 360 pad not found, the game will read the real pad"
  fi
  # Launched from EmulationStation, a quit signal reached the game at the end of loading and
  # SDL turned it into a quit request. Start it with SIGINT/SIGTERM ignored (SDL leaves ignored
  # signals alone); the exit hotkey still works because gptokeyb2 quits with SIGKILL.
  (
    trap '' INT TERM
    exec setsid env "${GAME_ENV[@]}" SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig" BOX64_SDL2_JGUID=1 \
      "$GAMEDIR/box64/box64" ./dontstarve
  )
  echo "[R36S] game exited with code $?"
  $ESUDO kill -9 $(pidof gptokeyb2) 2>/dev/null
else
  weston_dir=/tmp/weston
  $ESUDO mkdir -p "${weston_dir}"
  weston_runtime="weston_pkg_0.2"
  if [ ! -f "$controlfolder/libs/${weston_runtime}.squashfs" ]; then
    if [ ! -f "$controlfolder/harbourmaster" ]; then
      pm_message "This port requires the latest PortMaster to run, please go to https://portmaster.games/ for more info."
      sleep 5
      exit 1
    fi
    $ESUDO $controlfolder/harbourmaster --quiet --no-check runtime_check "${weston_runtime}.squashfs"
  fi
  if [[ "$PM_CAN_MOUNT" != "N" ]]; then
    $ESUDO umount "${weston_dir}"
  fi
  $ESUDO mount "$controlfolder/libs/${weston_runtime}.squashfs" "${weston_dir}"

  # gptokeyb2's virtual Xbox pad is broken on these CFWs: use keyboard/mouse there
  if [[ "$CFW_NAME" = "AmberELEC" ]] || [[ "$CFW_NAME" = "ArkOS AeUX" ]]; then
    $GPTOKEYB2 "dontstarve" -c "$GAMEDIR/dontstarve.gptk" &
  else
    $GPTOKEYB2 "dontstarve" -x &
  fi

  # box64's own profile for this game runs the bundled SDL2 emulated
  $ESUDO env CRUSTY_BLOCK_INPUT=1 $weston_dir/westonwrap.sh headless noop kiosk crusty_glx_gl4es \
    "${GAME_ENV[@]}" BOX64_EMULATED_LIBS=libSDL2-2.0.so.0 \
    "$GAMEDIR/box64/box64" ./dontstarve

  $ESUDO $weston_dir/westonwrap.sh cleanup
  if [[ "$PM_CAN_MOUNT" != "N" ]]; then
    $ESUDO umount "${weston_dir}"
  fi
fi

for e in "${GOVS[@]}"; do echo "${e#*=}" | $ESUDO tee "${e%%=*}" >/dev/null 2>&1; done
pm_finish

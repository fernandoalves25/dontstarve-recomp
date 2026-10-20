#!/bin/bash
# es_release.sh: run a game with EmulationStation closed, then bring EmulationStation back.
#
#   es_release.sh [--system <es system name>] <command> [args...]
#
# While a game runs, EmulationStation waits in the background holding 200-400 MB, a big
# share of a 1 GB handheld. ES runs its launch command inside its own systemd service, so
# stopping ES would kill the game too: the command is started as a separate transient unit
# instead, which waits for ES to exit, runs the command with the same environment, and always
# starts ES again afterwards, reopening it on --system. Outside ES (or if the unit can't be
# started) the command just runs normally.

system=""
if [ "$1" = "--system" ]; then
  system="$2"
  shift 2
fi

if [ -n "$ES_RELEASED" ] || ! systemctl is-active --quiet emulationstation; then
  ES_RELEASED=1 exec "$@"
fi

runner=$(mktemp /tmp/es_release.XXXXXX)
{
  echo '#!/bin/bash'
  export -p | grep -v '^declare -[a-z]*r'   # readonly ones (SHELLOPTS...) can't be re-set
  echo 'export ES_RELEASED=1'
  printf 'system=%q\n' "$system"
  cat << 'EOF'
restore() {
  [ -x /usr/local/bin/perfnorm ] && sudo perfnorm
  # Reopen ES on the system the game came from
  settings="$HOME/.emulationstation/es_settings.cfg"
  if [ -n "$system" ] && [ -f "$settings" ]; then
    sed -i '/<string name="StartupSystem"/d' "$settings"
    echo "<string name=\"StartupSystem\" value=\"$system\" />" >> "$settings"
  fi
  sudo systemctl start emulationstation
  rm -f "$0"
}
trap restore EXIT
# ES is being stopped right after this unit starts: wait until it has let go of the display
for _ in $(seq 150); do
  pgrep -f '^/usr/bin/emulationstation/emulationstation$' > /dev/null || break
  sleep 0.2
done
EOF
  printf 'cd %q\n' "$PWD"
  printf '%q ' "$@"
  echo
} > "$runner"
chmod 755 "$runner"

if sudo systemd-run --quiet --unit="es-release-$$" --uid="$(id -u)" --gid="$(id -g)" \
    --nice="$(nice)" /bin/bash "$runner"; then
  # The game now belongs to the unit. Stopping ES also ends this script and the ES launch
  # command that called it, since they run inside ES's service.
  sudo systemctl stop emulationstation
  exit 0
fi
rm -f "$runner"
ES_RELEASED=1 exec "$@"

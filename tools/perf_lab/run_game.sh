#!/bin/sh
# Runs build/macos/Halo/halo (or $HALO_BIN) once for a measurement, with the
# perf-lab's safe defaults (port/macos/README.md, "Perf lab"):
#
# - refuses to start while another Halo runs (someone may be playing, and
#   two games would share the system link ports);
# - no internet play or Tailscale (no invite links, nothing on the clipboard),
#   a window rather than fullscreen, hidden unless PERF_VISIBLE=1;
# - the saves in a folder of their own (a campaign's checkpoints must not
#   touch the player's), the maps from ~/Library/Application Support/Halo
#   through a link next to the executable;
# - always ends (HALO_EXIT_AFTER, 60 seconds unless set).
#
# The game's own settings pass through: HALO_MAP, HALO_PROFILE, HALO_FPS,
# HALO_COMMANDS, HALO_TEST_INPUT, HALO_NULL_RENDERER, HALO_STRESS, ...
#
#   HALO_MAP=b30 HALO_PROFILE=1 tools/perf_lab/run_game.sh
#
# Afterwards the logs are next to the executable: host.txt, debug.txt,
# profile.txt (HALO_PROFILE=1).
set -e
bin=${HALO_BIN:-build/macos/Halo/halo}
folder=$(cd "$(dirname "$bin")" && pwd)
if pgrep -f "MacOS/halo" > /dev/null || pgrep -f "Halo/halo$" > /dev/null; then
	echo "run_game.sh: a Halo is running; not starting another" >&2
	exit 2
fi
data="$HOME/Library/Application Support/Halo"
if [ ! -e "$folder/maps" ] && [ -d "$data/maps" ]; then
	ln -s "$data/maps" "$folder/maps"
fi
saves=${HALO_SAVE_ROOT:-${TMPDIR:-/tmp}/halo-perf-lab-saves}
mkdir -p "$saves"
export HALO_SAVE_ROOT="$saves"
export HALO_NET_ONLINE=false HALO_NET_TAILSCALE=false HALO_FULLSCREEN=false
export HALO_UPDATE_AUTO=false
export HALO_EXIT_AFTER=${HALO_EXIT_AFTER:-60}
if [ -z "$PERF_VISIBLE" ] && [ -z "$HALO_NULL_RENDERER" ]; then
	export HALO_HIDDEN_WINDOW=1
fi
# a hard stop too, should the game hang before its own
limit=$(printf '%.0f' "$HALO_EXIT_AFTER")
limit=$((limit + 60))
"$bin" > "$folder/stdout.txt" 2>&1 &
pid=$!
( sleep "$limit"; kill -9 "$pid" 2> /dev/null ) &
watchdog=$!
wait "$pid" || true
kill "$watchdog" 2> /dev/null || true
echo "run_game.sh: done ($folder)"

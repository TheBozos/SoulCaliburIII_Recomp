#!/usr/bin/env bash
# usage: sc3run.sh <name> <seconds> [extra env...]
# Runs the game for that long with a state dump every 5 s, logs to $SC3_RUNS/<name>.log and screenshots the window.
# The runner's user folder is $SC3_RUNS/user (memory cards and settings kept from one run to the next), never the
# player's; the cards next to the ELF are not copied into it.
set -u
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name=$1; secs=$2; shift 2
runs=${SC3_RUNS:-$root/build/runs}
runner=${SC3_RUNNER:-$root/build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner}
elf=$(realpath -s "${SC3_ELF:-$root/game/SLUS_212.16}")
mkdir -p "$runs/user"; cd "$runs"; runs=$PWD
elf_dir=$(dirname "$elf")
if command -v cygpath >/dev/null; then elf_dir=$(cygpath -w "$elf_dir"); fi
grep -qxF "$elf_dir" user/migrated.txt 2>/dev/null || echo "$elf_dir" >> user/migrated.txt
env PS2X_DEBUG_UI=0 PS2X_STATE_DUMP=5 PS2X_USER_DIR="$runs/user" PS2X_LOG_FILE=0 PS2X_NO_DIALOGS=1 "$@" \
  timeout $((secs + 15)) \
  "$runner" "$elf" > "$name.log" 2>&1 &
pid=$!
start=$(date +%s)
while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - start )) -lt "$secs" ]; do sleep 1; done
if kill -0 $pid 2>/dev/null; then
  geo=$(command -v hyprctl >/dev/null && hyprctl clients -j | python3 -I -c 'import json,sys
for c in json.load(sys.stdin):
    if c["title"].startswith("PS2-Recomp"):
        print("%d,%d %dx%d" % (c["at"][0], c["at"][1], c["size"][0], c["size"][1])); break')
  [ -n "$geo" ] && timeout 10 grim -g "$geo" "$name.png" && echo "shot $runs/$name.png"
  kill $pid 2>/dev/null
else
  echo "exited early"
fi
wait $pid 2>/dev/null
grep "^\[state\]   thread" "$name.log" | tail -4

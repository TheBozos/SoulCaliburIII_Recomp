#!/usr/bin/env bash
# compare_route.sh ROUTE OUT: one route of routes.py on the port (window, movies on, scene trace) and on PCSX2 (anchored on
# the port's scenes), then compare.py. Run it under a benchlock lease (benchlock take, then this): both windows go to
# workspace 9 unfocused.
#   SC3_BIN      the port's sc3 (window build); SC3_ELF, SC3_ISO the disc; SC3_2D_PACK etc. as for a play session
#   P2_DATA      PCSX2's private -datapath (set up once with p2drive.py setup)
#   SOUND=1      the port's sound into OUT/port.wav, PCSX2's into OUT/pcsx2/pcsx2_audio.raw (audio_compare.py)
#   CLOCKS       game counters anchored on too (default 4ED7F0,4B6BE4: the fight's stage loaded, the fight begun)
#   STEP=port|pcsx2|compare  one step only
set -euo pipefail
route=$1; out=$2
here=$(cd "$(dirname "$0")" && pwd)
clocks=${CLOCKS-4ED7F0,4B6BE4}
mkdir -p "$out"
script="$out/script.txt"; points="$out/points.txt"
python3 "$here/routes.py" script "$route" > "$script"
python3 "$here/routes.py" points "$route" > "$points"
stills=$(python3 "$here/routes.py" stills "$route")
last=$(tr ',' '\n' <<< "$stills" | sort -n | tail -1)
window=$(tr ',' '\n' <<< "$stills" | while read -r f; do seq $((f - 6)) $((f + 6)); done | sort -nu | paste -sd,)

if [ -z "${STEP:-}" ] || [ "$STEP" = port ]; then
  eval "$(benchlock env)"
  rm -rf "$out/port"; mkdir -p "$out/port/user"
  sound=(--no-sound); [ -n "${SOUND:-}" ] && sound=(--wav "$out/port.wav")
  (cd "$out/port" && SC3_TRACE_SCENE=1 SC3_TRACE_CLOCKS="$clocks" SC3_HOLD_FRAMES="$stills" PS2X_USER_DIR="$out/port/user" PS2X_NO_DIALOGS=1 PS2X_WINDOW_NO_FOCUS=1 \
    timeout $((last / 30 + 300)) "$SC3_BIN" --print --elf "$SC3_ELF" --iso "$SC3_ISO" --no-vsync "${sound[@]}" \
    --pad-script "$script" --capture-dir "$out/port/caps" --capture-frames "$window" --quit-after $((last + 60)) \
    > "$out/port.log" 2>&1)
fi
if [ -z "${STEP:-}" ] || [ "$STEP" = pcsx2 ]; then
  audio=(); [ -n "${SOUND:-}" ] && audio=(--sound)
  rm -rf "$out/pcsx2"
  python3 "$here/p2drive.py" run "$P2_DATA" "$out/pcsx2" --script "$script" --scenes "$out/port.log" --clocks "$clocks" --stills "$stills" \
    --until $((last + 30)) --timeout $((last / 30 + 600)) "${audio[@]}"
fi
if [ -z "${STEP:-}" ] || [ "$STEP" = compare ]; then
  "$here/compare.py" "$out/port/caps" "$out/pcsx2" "$points" --diff "$out/diff" | tee "$out/compare.md"
fi

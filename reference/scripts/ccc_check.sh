#!/usr/bin/env bash
# ccc_check.sh: Custom Character Creation on ccc_frames.txt (no movies), from boot to its sub-screens: the SAVE dialog over
# the CHARACTERS list, the "continue anyway" prompt, the slot list, the edit screen (the model in the arch), the "All"
# preset list open and its confirmation.
#
#   ccc_check.sh headless BUILD_DIR                  sc3-headless over the route: no FAULT, LOST 0, no 2D page dropped,
#                                                    [sc3:acknowledged] 0, the frame limit reached
#   ccc_check.sh stills BUILD_DIR OUT                sc3 (a window) holding and capturing the stills below into OUT
#   ccc_check.sh compare OUT BASELINE [MIN_PSNR]     each still of OUT against BASELINE's (both cropped to their 4:3
#                                                    picture, 640x448), exit 1 below MIN_PSNR (default 29 dB; two
#                                                    runs of one build give 31 dB and up)
#
# The baseline is a stills run of a build checked by eye against PCSX2; stills are game pictures and stay local.
# SC3_ELF and SC3_ISO as for the build; the window run starts sc3 itself (an agent wraps the call to keep it off screen).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SCRIPT="$HERE/ccc_frames.txt"
FRAMES=6800
STILLS=3400,3600,4200,5400,5850,6600
ELF="${SC3_ELF:-$ROOT/game/SLUS_212.16}"
iso=()
[ -z "${SC3_ISO:-}" ] || iso=(--iso "$SC3_ISO")

case "${1:-}" in
headless)
    build=${2:?usage: ccc_check.sh headless BUILD_DIR}
    user=$(mktemp -d)
    log="$user/run.log"
    SC3_NO_MOVIES=1 SC3_STATS=1 PS2X_USER_DIR="$user" PS2X_NO_DIALOGS=1 \
        "$build/sc3-headless" --elf "$ELF" "${iso[@]}" --frames "$FRAMES" --pad-script "$SCRIPT" > "$log" 2>&1 || true
    fail=0
    grep -q "frame limit $FRAMES reached" "$log" || { echo "ccc: the run did not reach frame $FRAMES"; fail=1; }
    if grep -q "FAULT" "$log"; then echo "ccc: FAULT"; fail=1; fi
    if grep -qE "LOST [1-9]" "$log"; then echo "ccc: mesh parts lost"; fail=1; fi
    if grep -qE " [1-9][0-9]* pages dropped" "$log"; then echo "ccc: 2D pages dropped"; fail=1; fi
    grep -q "\[sc3:acknowledged\] 0 hits" "$log" || { echo "ccc: device registers reached"; fail=1; }
    if [ "$fail" = 0 ]; then echo "ccc headless: ok"; rm -rf "$user"; else echo "ccc headless: failed, log $log"; fi
    exit "$fail"
    ;;
stills)
    build=${2:?usage: ccc_check.sh stills BUILD_DIR OUT}; out=${3:?usage: ccc_check.sh stills BUILD_DIR OUT}
    rm -rf "$out/user" && mkdir -p "$out/user"
    SC3_NO_MOVIES=1 SC3_HOLD_FRAMES="$STILLS" PS2X_USER_DIR="$out/user" PS2X_NO_DIALOGS=1 PS2X_WINDOW_NO_FOCUS=1 \
        "$build/sc3" --elf "$ELF" "${iso[@]}" --no-sound --width 640 --height 480 --pad-script "$SCRIPT" \
        --capture-dir "$out" --capture-frames "$STILLS" --quit-after $((${STILLS##*,} + 50))
    ;;
compare)
    out=${2:?usage: ccc_check.sh compare OUT BASELINE [MIN_PSNR]}; base=${3:?usage: ccc_check.sh compare OUT BASELINE [MIN_PSNR]}
    min=${4:-29}
    work=$(mktemp -d)
    norm() {
        local size w h cw ch
        size=$(magick identify -format '%w %h' "$1")
        w=${size% *} h=${size#* }
        if [ $((w * 3)) -gt $((h * 4)) ]; then cw=$((h * 4 / 3)) ch=$h; else cw=$w ch=$((w * 3 / 4)); fi
        magick "$1" -gravity center -crop "${cw}x${ch}+0+0" +repage -filter Triangle -resize '640x448!' -alpha off "$2"
    }
    still() { { ls "$1"/frame_*.png 2>/dev/null || true; } | awk -v n="$2" '{ m = $0; sub(/.*frame_0*/, "", m); sub(/\.png$/, "", m);
        d = m - n; if (d < 0) d = -d; if (d <= 2 && (best == "" || d < bd)) { best = $0; bd = d } } END { print best }'; }
    fail=0
    for n in ${STILLS//,/ }; do
        a=$(still "$out" "$n"); b=$(still "$base" "$n")
        if [ -z "$a" ] || [ -z "$b" ]; then echo "frame $n: missing still"; fail=1; continue; fi
        norm "$a" "$work/a.png"; norm "$b" "$work/b.png"
        line=$(python3 -I "$ROOT/scripts/imgdiff.py" "$work/a.png" "$work/b.png" --diff "$work/diff_$n.png" --min-psnr "$min") || fail=1
        echo "frame $n: $line"
    done
    [ "$fail" = 0 ] && echo "ccc stills: ok" || echo "ccc stills: failed (differences in $work)"
    exit "$fail"
    ;;
*)
    sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac

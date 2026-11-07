#!/usr/bin/env bash
# Builds SC3 on BozoEngine into build/bozo: sc3, sc3-headless, sc3-check, from game/SLUS_212.16 (SC3_ELF overrides it)
# and the disc image SC3_ISO when set. Options: --no-window (no sc3, no SDL3), --target <target> (repeatable),
# --engine <BozoEngine tree>, --build-type <type>. A player's build from the disc: scripts/sc3_build.py.
source "$(dirname "$0")/common.sh"
out="$ROOT/build/bozo"
window=ON
engine="$ROOT/BozoEngine"
type=Release
targets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-window) window=OFF ;;
        --target) targets+=(--target "$2"); shift ;;
        --engine) engine=$(realpath "$2"); shift ;;
        --build-type) type=$2; shift ;;
        *) echo "bozo_build.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -f "$engine/CMakeLists.txt" ] || { echo "no BozoEngine at $engine (git submodule update --init BozoEngine, or --engine)" >&2; exit 1; }
cmake -S "$ROOT" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE="$type" -DBOZO_ENGINE_DIR="$engine" \
    -DSC3_ELF="$(realpath "${SC3_ELF:-$ELF}")" -DSC3_WINDOW="$window" -DSC3_ISO="${SC3_ISO:+$(realpath "$SC3_ISO")}" \
    -DSC3_SYMBOL_MAP="${SC3_SYMBOL_MAP:+$(realpath "$SC3_SYMBOL_MAP")}" || exit 1
exec cmake --build "$out" -j "$JOBS" "${targets[@]}"

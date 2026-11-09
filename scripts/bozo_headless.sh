#!/usr/bin/env bash
# Runs sc3-headless (build/bozo) on game/SLUS_212.16 (SC3_ELF overrides it) with a fresh user folder under
# build/bozo-runs/<name>. Extra args go to sc3-headless (--frames n, --iso image, --out run.ref ...).
# Usage: scripts/bozo_headless.sh <name> [args...]
source "$(dirname "$0")/common.sh"
name=${1:?usage: bozo_headless.sh <name> [args...]}; shift
BIN="$ROOT/build/bozo/sc3-headless"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/bozo_build.sh first"; exit 1; }
out="$ROOT/build/bozo-runs/$name"
rm -rf "$out" && mkdir -p "$out/user"
PS2X_USER_DIR="$out/user" PS2X_NO_DIALOGS=1 PS2X_WINDOW_NO_FOCUS=1 "$BIN" --elf "${SC3_ELF:-$ELF}" "$@" 2>&1 | tee "$out/run.log"

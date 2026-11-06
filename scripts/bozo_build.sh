#!/usr/bin/env bash
# Builds SC3 on BozoEngine into build/bozo (scripts/sc3_build.py): sc3, sc3-headless, sc3-check, from game/SLUS_212.16
# (SC3_ELF overrides it) and the disc image SC3_ISO when set. Extra args go to sc3_build.py (--no-window, --target ...).
source "$(dirname "$0")/common.sh"
args=(--elf "${SC3_ELF:-$ELF}" --out "$ROOT/build/bozo" --jobs "$JOBS")
[ -z "${SC3_ISO:-}" ] || args+=(--iso "$SC3_ISO")
[ -z "${SC3_SYMBOL_MAP:-}" ] || args+=(--symbol-map "$SC3_SYMBOL_MAP")
exec "${PYTHON:-python3}" -I "$ROOT/scripts/sc3_build.py" "${args[@]}" "$@"

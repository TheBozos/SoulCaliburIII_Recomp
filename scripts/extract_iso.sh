#!/usr/bin/env bash
# Extracts the files of your own Soulcalibur III disc image (NTSC-U SLUS-21216, PAL SCES-53312 or Korea SCKA-20059) into game/.
# Usage: scripts/extract_iso.sh <image.iso>
source "$(dirname "$0")/common.sh"
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <image.iso>"; exit 1; }
command -v bsdtar >/dev/null || { echo "bsdtar not found (libarchive)"; exit 1; }
mkdir -p "$GAME_DIR"
rm -f "$GAME_DIR/SLUS_212.16" "$GAME_DIR/SCES_533.12" "$GAME_DIR/SCKA_200.59"
bsdtar -xf "$1" -C "$GAME_DIR" --exclude PAD_0.DAT
chmod -R u+w "$GAME_DIR"
if [ -f "$GAME_DIR/SLUS_212.16" ]; then
    ELF="$GAME_DIR/SLUS_212.16" expected=9bbb67a70137052931bc245855eedeee6bbe5b49
elif [ -f "$GAME_DIR/SCES_533.12" ]; then
    ELF="$GAME_DIR/SCES_533.12" expected=8c79c1db3395e72ada118860433de6638ccf9401
elif [ -f "$GAME_DIR/SCKA_200.59" ]; then
    ELF="$GAME_DIR/SCKA_200.59" expected=b9f3958b154d468105d3351e62057634645cec2a
else
    echo "no SLUS_212.16, SCES_533.12 or SCKA_200.59 in $1: not a Soulcalibur III disc the port supports?"; exit 1
fi
actual=$(sha1sum "$ELF" | cut -d' ' -f1)
[ "$actual" = "$expected" ] || echo "warning: $(basename "$ELF") SHA-1 is $actual, the port is built against $expected"
echo "Disc files in $GAME_DIR"

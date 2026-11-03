#!/usr/bin/env bash
# Launches the recompiled game with game/ as cdrom0:. Extra args are passed to ps2EntryRunner.
# The SPU2 follows the host's clock unless PS2X_SPU2_WALLCLOCK=0. SC3_TEXTURE_PACK_MODE=1 draws the direct draws'
# textures from build/sc3_textures.pack (SC3_TEXTURE_PACK names another; off by default, a diagnostic for now).
source "$(dirname "$0")/common.sh"
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -n "${SC3_TEXTURE_PACK+x}" ] || [ ! -f "$TEXTURE_PACK" ] || export SC3_TEXTURE_PACK="$TEXTURE_PACK"
[ -n "${SC3_MESH_PACK+x}" ] || [ ! -f "$MESH_PACK" ] || export SC3_MESH_PACK="$MESH_PACK"
[ -n "${SC3_2D_PACK+x}" ] || [ ! -f "$TWO_D_PACK" ] || export SC3_2D_PACK="$TWO_D_PACK"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
exec "$BIN" "$ELF" "$@"

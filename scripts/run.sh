#!/usr/bin/env bash
# Launches the recompiled game with game/ as cdrom0:. Extra args are passed to ps2EntryRunner.
# The SPU2 follows the host's clock unless PS2X_SPU2_WALLCLOCK=0. The 3D is drawn from build/sc3_meshes.pack and
# build/sc3_textures.pack when they are there (SC3_MESH_PACK / SC3_TEXTURE_PACK name others; SC3_NATIVE_MESHES=0
# and SC3_TEXTURE_PACK_MODE=0 keep the old path for diagnosis).
source "$(dirname "$0")/common.sh"
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -n "${SC3_TEXTURE_PACK+x}" ] || [ ! -f "$TEXTURE_PACK" ] || export SC3_TEXTURE_PACK="$TEXTURE_PACK"
[ -n "${SC3_MESH_PACK+x}" ] || [ ! -f "$MESH_PACK" ] || export SC3_MESH_PACK="$MESH_PACK"
[ -n "${SC3_2D_PACK+x}" ] || [ ! -f "$TWO_D_PACK" ] || export SC3_2D_PACK="$TWO_D_PACK"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
exec "$BIN" "$ELF" "$@"

#!/usr/bin/env bash
# Builds ps2EntryRunner with the generated code linked in -> build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner, and the
# packs from the disc files in game/ when there are none yet: build/sc3_textures.pack and .keys (about 10 minutes on
# 6 cores), build/sc3_meshes.pack, build/sc3_2d.pack (+ _textures.pack, .leaves; the native 2D's).
# Usage: scripts/build_runtime.sh [Release|RelWithDebInfo|Debug]
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-RelWithDebInfo}"
[ -f "$GENERATED/register_functions.cpp" ] || { echo "no generated code, run scripts/recomp.sh first"; exit 1; }
compiler=()
if [ ! -f "$GAME_BUILD/CMakeCache.txt" ] && [ -z "${CXX:-}" ] && command -v clang++ >/dev/null; then
    compiler=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
fi
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPS2X_BUILD_STUDIO=OFF "${compiler[@]}"
cmake --build "$GAME_BUILD" --target ps2EntryRunner -j "$JOBS"
case "$(uname -s)" in MINGW* | MSYS*) "$ROOT/scripts/stage_dlls.sh" "$GAME_BUILD" ;; esac
if [ ! -f "$TEXTURE_PACK" ] && [ -f "$GAME_DIR/ROOT.OLK" ]; then
    "${PYTHON:-python3}" -I "$ROOT/scripts/sc3_texture_pack.py" "$GAME_DIR" "$TEXTURE_PACK"
fi
if [ ! -f "$MESH_PACK" ] && [ -f "$GAME_DIR/ROOT.OLK" ]; then
    "${PYTHON:-python3}" -I "$ROOT/scripts/sc3_mesh_pack.py" "$GAME_DIR" "$MESH_PACK"
fi
if [ ! -f "$TWO_D_PACK" ] && [ -f "$GAME_DIR/ROOT.OLK" ]; then
    "${PYTHON:-python3}" -I "$ROOT/scripts/sc3_2d_pack.py" "$GAME_DIR" "$TWO_D_PACK"
fi

#!/bin/bash
# A portable folder from a Windows build tree (cmake/mingw-cross.cmake): sc3.exe, sc3-headless.exe, the symbol map and
# the packs beside them, game/ left for the player's own SLUS_212.16 and disc image. It holds code generated from the
# disc and packs converted from it: it stays on the machine that built it.
#   scripts/make_win_package.sh <build tree> <out folder>
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
build=$1; out=$2
mkdir -p "$out/packs" "$out/game"
strip=${MINGW_ROOT:+$MINGW_ROOT/bin/x86_64-w64-mingw32-}strip
for program in sc3.exe sc3-headless.exe; do
    "$strip" -o "$out/$program" "$build/$program"
done
cp "$root/recomp/sc3_symbols.txt" "$out/"
for pack in sc3_2d.pack sc3_2d.leaves sc3_2d_textures.pack sc3_textures.pack sc3_textures.keys sc3_meshes.pack sc3_meshes.leaves; do
    [ ! -f "$build/packs/$pack" ] || ln -f "$build/packs/$pack" "$out/packs/$pack" 2>/dev/null || cp "$build/packs/$pack" "$out/packs/$pack"
done

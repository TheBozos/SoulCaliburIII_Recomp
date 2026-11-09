#!/usr/bin/env bash
# Records one reference run on the reference runner (reference/README.md): a fresh user folder, the env file's variables,
# the log and outputs in $SC3_REF_OUT/<name>[.ref|.clg|.log]. A game run: take the lock around it.
#   SC3_REF_RUNNER=<ps2EntryRunner> SC3_ELF=<SLUS_212.16> SC3_REF_PACKS=<folder of the packs> SC3_REF_OUT=<folder> \
#     SC3_REF_CALL_NAMES=<scripts/sc3_call_names.py's output> reference/scripts/run.sh <name> <seconds> <env file>
# An env file: one VAR=value a line; {OUT} is <out>/<name>, @file as a value reads it from this folder.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
name=$1; secs=$2; envfile=$3
out_dir=${SC3_REF_OUT:?}
OUT=$out_dir/$name
user=$OUT.user
rm -rf "$user" "$OUT.ref" "$OUT.ref.calls" "$OUT.clg" "$OUT.log"
mkdir -p "$user" "$OUT"
elf=$(realpath -s "${SC3_ELF:?}")
dirname "$elf" > "$user/migrated.txt"
export PS2X_USER_DIR="$user" PS2X_NO_DIALOGS=1 PS2X_WINDOW_NO_FOCUS=1 PS2X_PAD_NO_HOST=1 PS2X_DEBUG_UI=0 PS2X_LOG_FILE=0
export SC3_TEXTURE_PACK="${SC3_REF_PACKS:?}/sc3_textures.pack" SC3_2D_PACK="$SC3_REF_PACKS/sc3_2d.pack"
export SC3_CALL_NAMES="${SC3_REF_CALL_NAMES:?}"
while IFS= read -r line || [ -n "$line" ]; do
    [ -z "$line" ] && continue
    var=${line%%=*}; val=${line#*=}
    if [ "${val:0:1}" = "@" ]; then val=$(cat "$here/${val:1}"); fi
    val=${val//\{OUT\}/$OUT}
    export "$var=$val"
done < "$here/$envfile"
timeout "$secs" "${SC3_REF_RUNNER:?}" "$elf" > "$OUT.log" 2>&1 && code=0 || code=$?
echo "exit $code"

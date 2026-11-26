set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SLUS_212.16"
[ -f "$ELF" ] || [ ! -f "$GAME_DIR/SCES_533.12" ] || ELF="$GAME_DIR/SCES_533.12"
[ -f "$ELF" ] || [ ! -f "$GAME_DIR/SCKA_200.59" ] || ELF="$GAME_DIR/SCKA_200.59"
JOBS="${JOBS:-$(nproc)}"

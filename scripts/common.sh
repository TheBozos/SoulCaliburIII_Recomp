set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SLUS_212.16"
JOBS="${JOBS:-$(nproc)}"

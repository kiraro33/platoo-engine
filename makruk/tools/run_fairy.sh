#!/usr/bin/env bash
# Start the Fairy-Stockfish oracle with Makruk selected and the Makruk NNUE net loaded,
# then forward stdin to it. Works from an MSYS2 shell (or Git Bash, for running only).
#
#   makruk/tools/run_fairy.sh                       # interactive UCI session
#   printf 'position startpos\ngo perft 5\n' | makruk/tools/run_fairy.sh
#   makruk/tools/run_fairy.sh --no-net              # classical eval
#
# Fairy-Stockfish lives in the git-ignored external/ folder of the repository.
# Override with MAKRUK_EXTERNAL.
set -eu

EXTERNAL="${MAKRUK_EXTERNAL:-$(cd "$(dirname "$0")/../.." && pwd)/external}"
ENGINE="$EXTERNAL/Fairy-Stockfish/src/stockfish.exe"
[ -x "$ENGINE" ] || ENGINE="$EXTERNAL/Fairy-Stockfish/src/stockfish"
NET="$EXTERNAL/nets/makruk-a8c621e24a8c.nnue"

USE_NET=1
[ "${1:-}" = "--no-net" ] && USE_NET=0

[ -x "$ENGINE" ] || { echo "Fairy-Stockfish not found under $EXTERNAL (set MAKRUK_EXTERNAL)" >&2; exit 1; }

# Fairy-SF on Windows needs a native path for EvalFile.
NET_ARG="$NET"
command -v cygpath >/dev/null 2>&1 && NET_ARG="$(cygpath -m "$NET")"

{
  echo "setoption name UCI_Variant value makruk"
  if [ "$USE_NET" = 1 ]; then
    [ -f "$NET" ] || { echo "Makruk net not found: $NET" >&2; exit 1; }
    echo "setoption name EvalFile value $NET_ARG"
  fi
  cat
} | "$ENGINE"

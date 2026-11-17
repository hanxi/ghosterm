#!/usr/bin/env bash
# Both channels and the Sparkle driver are tested in an isolated source copy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/mutation-check-app-update.py" "$@"

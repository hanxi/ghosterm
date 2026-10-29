#!/usr/bin/env bash
# Preserve the existing entry point; all mutations run in an isolated core copy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/mutation-check-known-hosts.py" "$@"

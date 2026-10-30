#!/usr/bin/env bash
# Test-only Python environment, never a product dependency.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/tmp"
export TMPDIR="$ROOT/tmp"
export PIP_CACHE_DIR="$ROOT/build/ssh-channel-toolchain/pip-cache"
python3 -m venv "$ROOT/build/ssh-channel-toolchain"
"$ROOT/build/ssh-channel-toolchain/bin/python" -m pip install -r "$ROOT/scripts/fixtures/ssh-channel-requirements.txt"

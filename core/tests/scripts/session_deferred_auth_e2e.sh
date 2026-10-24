#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/tmp"
work="$(mktemp -d "$ROOT/tmp/deferred-auth-XXXXXX")"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT INT TERM
ssh-keygen -t ed25519 -N '' -f "$work/wrong-key" -q </dev/null
export GT_DEFER_BAD_KEY="$work/wrong-key"
bash "$ROOT/core/tests/scripts/ssh_e2e.sh" "$1"

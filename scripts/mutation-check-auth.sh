#!/usr/bin/env bash
# 真实 sshd + 共享异步认证；变异只落隔离 Swift 副本。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-auth
mkdir -p "$ROOT/tmp"
WRAPPER="$(mktemp "$ROOT/tmp/auth-flow-run-XXXXXX")"
trap 'rm -f "$WRAPPER"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 - "$ROOT" "$WRAPPER" <<'PY'
import pathlib,shlex,sys
root,wrapper=map(pathlib.Path,sys.argv[1:])
wrapper.write_text('#!/bin/sh\nexec python3 '+shlex.quote(str(root/'scripts/mutation-check-auth-flow.py'))+' "$@"\n')
wrapper.chmod(0o700)
PY
export TMPDIR="$ROOT/tmp"
gt_timeout 900 bash "$ROOT/core/tests/scripts/ssh_e2e.sh" "$WRAPPER"

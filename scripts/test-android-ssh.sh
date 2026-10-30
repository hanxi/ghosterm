#!/usr/bin/env bash
# Real isolated host sshd reached through adb reverse; no private key is stored on Android.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require android-ssh
: "${ANDROID_SERIAL:?Set ANDROID_SERIAL to the test device}"
mkdir -p "$ROOT/tmp"
WRAPPER="$(mktemp "$ROOT/tmp/android-ssh-run-XXXXXX")"
trap 'rm -f "$WRAPPER"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
python3 - "$ROOT" "$WRAPPER" <<'PY'
import pathlib,shlex,sys
root,wrapper=map(pathlib.Path,sys.argv[1:])
wrapper.write_text('#!/bin/sh\nexec python3 '+shlex.quote(str(root/'scripts/test-android-ssh.py'))+' "$@"\n')
wrapper.chmod(0o700)
PY
export TMPDIR="$ROOT/tmp"
gt_timeout 1800 bash "$ROOT/core/tests/scripts/ssh_e2e.sh" "$WRAPPER" > "$ROOT/tmp/android-ssh-fixture.log" 2>&1 || {
    cat "$ROOT/tmp/android-ssh-fixture.log"; exit 1;
}
cat "$ROOT/tmp/android-ssh-fixture.log"
if ! rg -q '^ANDROID SSH PASS' "$ROOT/tmp/android-ssh-fixture.log" || rg -q 'SKIP' "$ROOT/tmp/android-ssh-fixture.log"; then
    echo 'Android SSH fixture did not complete the device tests' >&2; exit 1
fi

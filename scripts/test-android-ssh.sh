#!/usr/bin/env bash
# Isolated sshd via adb reverse; test PEM stays in memory or the Keystore-encrypted container.
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
CHANNEL_PYTHON=python3
if [[ "${ANDROID_SSH_ALL:-0}" == 1 || "${ANDROID_SSH_CHANNEL_MUTATE:-0}" == 1 || "${ANDROID_SSH_TEST_CLASS:-}" == *SSHChannelTest* || "${ANDROID_SSH_TEST_CLASS:-}" == *RemoteFilesTest* || "${ANDROID_FILES_OWNER_MUTATE:-0}" == 1 || "${ANDROID_FILES_PRODUCT_MUTATE:-0}" == 1 || "${ANDROID_SSH_TEST_CLASS:-}" == *RemoteFilesProductTest* ]]; then
    CHANNEL_PYTHON="$ROOT/build/ssh-channel-toolchain/bin/python"
    if [[ ! -x "$CHANNEL_PYTHON" ]]; then
        echo 'Run bash scripts/bootstrap-ssh-channel-tests.sh first' >&2; exit 1
    fi
fi
python3 - "$ROOT" "$WRAPPER" "$CHANNEL_PYTHON" <<'PY'
import pathlib,shlex,sys
root,wrapper=map(pathlib.Path,sys.argv[1:3])
wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(sys.argv[3])+' '+shlex.quote(str(root/'scripts/test-android-ssh.py'))+' "$@"\n')
wrapper.chmod(0o700)
PY
export TMPDIR="$ROOT/tmp"
export PYTHONPYCACHEPREFIX="$ROOT/tmp/android-python-cache"
gt_timeout 1800 bash "$ROOT/core/tests/scripts/ssh_e2e.sh" "$WRAPPER" > "$ROOT/tmp/android-ssh-fixture.log" 2>&1 || {
    cat "$ROOT/tmp/android-ssh-fixture.log"; exit 1;
}
cat "$ROOT/tmp/android-ssh-fixture.log"
if ! rg -q '^ANDROID SSH PASS' "$ROOT/tmp/android-ssh-fixture.log" || rg -q 'SKIP' "$ROOT/tmp/android-ssh-fixture.log"; then
    echo 'Android SSH fixture did not complete the device tests' >&2; exit 1
fi

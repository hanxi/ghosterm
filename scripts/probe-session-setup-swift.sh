#!/usr/bin/env bash
# Real native Swift resolver/authenticator/setup, generated disposable sshd keys.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/tmp"
DRIVER="$ROOT/tmp/swift-session-setup-driver.sh"
cat > "$DRIVER" <<'DRIVER'
#!/usr/bin/env bash
set -euo pipefail
export GHOSTERM_SETUP_HOST="$1" GHOSTERM_SETUP_PORT="$2" GHOSTERM_SETUP_USER="$3"
export GHOSTERM_SETUP_KEY="$4" GHOSTERM_SETUP_WORK="$5"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
xcrun swift test --package-path "$ROOT/apple/GhostermCore" --filter SessionSetupTests.testRealSSHSetup
DRIVER
chmod 700 "$DRIVER"
LOG="$ROOT/tmp/swift-session-setup.log"
bash "$ROOT/core/tests/scripts/ssh_e2e.sh" "$DRIVER" >"$LOG" 2>&1
cat "$LOG"
grep -q 'PASS native async resolver/authenticator/take/Linux capability/cancellation' "$LOG"

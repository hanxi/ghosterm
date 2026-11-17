# Sourced by the two Apple gates. CI full explicitly selects required.
APPLE_GATE_MODE="${GHOSTERM_APPLE_GATE_MODE:-optional}"
APPLE_GATE_PARTIAL=0
export TMPDIR="$TMP"
case "$APPLE_GATE_MODE" in
  required|optional) ;;
  *) echo "Apple gate: GHOSTERM_APPLE_GATE_MODE must be required or optional" >&2; exit 2 ;;
esac

apple_gate_missing() {
  if [ "$APPLE_GATE_MODE" = required ]; then
    echo "Apple gate required: $*" >&2
    exit 2
  fi
  echo "SKIP Apple gate optional: $*"
  APPLE_GATE_PARTIAL=1
}

apple_gate_log() {
  local logfile="$1"
  shift
  if [ "$APPLE_GATE_MODE" = required ]; then
    python3 "$ROOT/scripts/check-required-xctests.py" "$logfile" "$@" || exit 1
  else
    python3 "$ROOT/scripts/check-required-xctests.py" "$logfile" || exit 1
    if grep -Eq "Test Case .* skipped" "$logfile"; then APPLE_GATE_PARTIAL=1; fi
  fi
}

apple_gate_finish() {
  if [ "$APPLE_GATE_PARTIAL" = 1 ]; then
    echo "$1 PARTIAL (mode=$APPLE_GATE_MODE; skipped scope is unverified)"
  else
    echo "$1 PASS (mode=$APPLE_GATE_MODE)"
  fi
}

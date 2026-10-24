#!/usr/bin/env bash
# 多值指令「一行多 token」的完整输出（含 stderr / 退出码）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/tmp"
WORK=$(mktemp -d "$ROOT/tmp/gt-m37b-XXXXXX")

run() {
  local name="$1" body="$2" alias="$3"
  printf '%s\n' "$body" > "$WORK/$name.conf"
  echo "### $name"
  out=$(ssh -G -F "$WORK/$name.conf" "$alias" 2>&1); rc=$?
  echo "    rc=$rc"
  printf '%s\n' "$out" | grep -vE '^(identityfile ~/\.ssh/id_)' | sed 's/^/    /' | head -25
  echo
}

run if_oneline_two 'Host m
  IdentityFile ~/.ssh/a ~/.ssh/b
' m

run lf_oneline_two 'Host m
  LocalForward 8080 h:80 9090 h:90
' m

run setenv_oneline_two 'Host m
  SetEnv FOO=bar BAZ=qux
' m

run unknown_dup 'Host m
  User u1
  User u2
  VisualHostKey yes
  VisualHostKey no
' m

rm -rf "$WORK"

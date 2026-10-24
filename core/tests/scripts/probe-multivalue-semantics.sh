#!/usr/bin/env bash
# M3-7 语义对拍：多值指令「一行多 token」vs「多行」在 ssh -G 下的生效语义。
# 目的：确认无损写回把累积字段规范化为「一行一条目」是否语义等价。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
mkdir -p "$ROOT/tmp"
WORK=$(mktemp -d "$ROOT/tmp/gt-m37-XXXXXX")

run() { # run <name> <conf-body> <alias>
  local name="$1" body="$2" alias="$3"
  printf '%s\n' "$body" > "$WORK/$name.conf"
  echo "### $name (alias=$alias)"
  ssh -G -F "$WORK/$name.conf" "$alias" 2>&1 \
    | grep -E '^(identityfile|setenv|localforward|remoteforward|user|hostname) ' \
    | sed 's/^/    /'
  echo
}

echo "OpenSSH: $(ssh -V 2>&1)"
echo

run if_oneline_two_tokens 'Host m
  IdentityFile ~/.ssh/a ~/.ssh/b
' m

run if_two_lines 'Host m
  IdentityFile ~/.ssh/a
  IdentityFile ~/.ssh/b
' m

run setenv_oneline_two_tokens 'Host m
  SetEnv FOO=bar BAZ=qux
' m

run setenv_two_lines 'Host m
  SetEnv FOO=bar
  SetEnv BAZ=qux
' m

run setenv_global_first_wins 'SetEnv GLOBAL=1
Host m
  SetEnv FOO=bar
' m

run lf_oneline_two_tokens 'Host m
  LocalForward 8080 h:80 9090 h:90
' m

run lf_two_lines 'Host m
  LocalForward 8080 h:80
  LocalForward 9090 h:90
' m

run rf_oneline_two_tokens 'Host m
  RemoteForward 8080 h:80 9090 h:90
' m

run rf_two_lines 'Host m
  RemoteForward 8080 h:80
  RemoteForward 9090 h:90
' m

# 未知指令重复行（extra 原地保留的语义基线：first-wins）
run unknown_dup 'Host m
  VisualHostKey yes
  VisualHostKey no
' m

rm -rf "$WORK"

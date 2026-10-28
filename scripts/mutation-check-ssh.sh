#!/usr/bin/env bash
# 真实 sshd；每个变异隔离编译，目标断言红、配置对照绿并核对 SHA 回滚。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-ssh
mkdir -p "$ROOT/tmp"
exec python3 "$ROOT/scripts/mutation-check-ssh-core.py" ssh

#!/usr/bin/env bash
# 保留原八组 SFTP 变异；隔离副本、真 sshd、独立配置对照与 SHA 回滚。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-sftp
mkdir -p "$ROOT/tmp"
exec python3 "$ROOT/scripts/mutation-check-ssh-core.py" sftp

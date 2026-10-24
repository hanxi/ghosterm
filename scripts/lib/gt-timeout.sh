#!/usr/bin/env bash
# GNU `timeout` 在 macOS 上不一定叫 `timeout`：brew coreutils 装的是 `gtimeout`，
# 而 GitHub 的 macos-15 runner 两个都可能没有。
#
# 为什么变异验证必须区分「命令不存在」和「断言变红」：两者都表现为非零退出码，
# 于是**基线红、变异也红**，`expect red` 全部假通过，看起来「有牙」其实什么都没测
# （CI 实测踩过：mutation-check-transport 报 4 passed / 8 failed，每项「红」只花
#  2–4 秒，而同一 job 里 ctest 是绿的）。铁律 #6：禁止假装成功，也禁止假装失败。
#
# 用法：
#   . "$ROOT/scripts/lib/gt-timeout.sh"
#   gt_timeout_require mutation-check-tabs     # 解析不到就 exit 2，拒绝起跑
#   gt_timeout 300 swift test --filter X       # 代替裸 `timeout 300 …`
# 这里**不降级成「不设超时」**：P4（写队列满时无限等）那类变异靠超时才有界，
# 拿掉超时就是拿掉判据。

gt_timeout_bin=""

gt_timeout_resolve() {
  local candidate
  for candidate in timeout gtimeout; do
    if command -v "$candidate" >/dev/null 2>&1; then
      gt_timeout_bin="$candidate"
      return 0
    fi
  done
  return 1
}

gt_timeout_require() {
  gt_timeout_resolve && return 0
  echo "${1:-本脚本}：找不到 timeout/gtimeout——拒绝起跑（宁可红，不拿「命令不存在」冒充变异红）。" >&2
  echo "  macOS：brew install coreutils（提供 gtimeout）。" >&2
  exit 2
}

gt_timeout() {
  local secs=$1
  shift
  if [ -z "$gt_timeout_bin" ]; then
    gt_timeout_require "gt_timeout"
  fi
  "$gt_timeout_bin" "$secs" "$@"
}

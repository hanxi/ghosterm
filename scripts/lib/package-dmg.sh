# DMG verification owns only its mountpoint under the package work directory.
# A busy volume must be detached before any recursive work-directory removal.
gt_package_is_unmounted() {
  python3 -c 'import os,sys; sys.exit(1 if os.path.ismount(sys.argv[1]) else 0)' "$1"
}

gt_package_detach_dmg() {
  local mount=$1
  [ -n "$mount" ] || return 0
  gt_package_is_unmounted "$mount" && return 0
  if ! hdiutil detach "$mount" >/dev/null 2>&1; then
    echo "普通卸载失败，强制卸载本次验包挂载：${mount}" >&2
    hdiutil detach -force "$mount" >/dev/null 2>&1 || return 1
  fi
  gt_package_is_unmounted "$mount"
}

gt_package_cleanup_work() {
  local work=$1 mount=$2 keep=$3
  if ! gt_package_detach_dmg "$mount"; then
    echo "package-mac: dmg 仍挂载，保留临时目录：${work}" >&2
    return 1
  fi
  [ "$keep" -eq 1 ] || rm -rf -- "$work"
}

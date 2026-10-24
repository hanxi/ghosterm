#!/usr/bin/env bash
# 变异验证：证明**拖放路由 / 预览路径 / 完成回调**的断言有牙（无网可跑）。
#
#   D1 DropRouter 把「远端拖到本地」判成上传（方向反了）；
#   D2 DragItem.decode 不还原转义（含换行/反斜杠的路径被拆坏）；
#   D3 完成回调（onFinish）不触发（Quick Look 预览永不弹）。
#
# 纪律照 AGENTS.md：起跑扫残留标记；trap 复原；回滚比对 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-dragdrop

TMP="$ROOT/tmp"; mkdir -p "$TMP"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0

FILES=(
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/FilesDragDrop.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TransferManager.swift"
)
declare -a BACKUPS=() BASE_SHAS=()
restore_all() { local i=0; for f in "${FILES[@]}"; do
    [ -n "${BACKUPS[$i]:-}" ] && [ -f "${BACKUPS[$i]}" ] && cp "${BACKUPS[$i]}" "$f"; i=$((i+1)); done; }
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then echo "残留标记（$f）——拒绝（exit 2）" >&2; exit 2; fi
done
i=0; for f in "${FILES[@]}"; do b="$(mktemp "$TMP/gt-mutdd-XXXXXX")"; cp "$f" "$b"; BACKUPS[$i]="$b"; BASE_SHAS[$i]="$(shasum -a 256 "$f" | awk '{print $1}')"; i=$((i+1)); done

run_test() { ( cd "$ROOT/apple/GhostermApp" && gt_timeout 900 swift test --filter FilesDragDropTests >/dev/null 2>&1 ); }
expect_green() { if run_test; then echo "  ✓ $1 → green"; PASS=$((PASS+1)); else echo "  ✗ $1 → 期望 green，实际 red"; FAIL=$((FAIL+1)); fi; }
expect_red() { if run_test; then echo "  ✗ $1 → 期望 red，实际 green（断言无牙）"; FAIL=$((FAIL+1)); else echo "  ✓ $1 → red"; PASS=$((PASS+1)); fi; }
mutate() { python3 "$1" "$2"; }
rollback() { local i=0; for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"
  [ "$(shasum -a 256 "$f" | awk '{print $1}')" = "${BASE_SHAS[$i]}" ] || { echo "  ✗ 回滚 sha256 不一致"; FAIL=$((FAIL+1)); }; i=$((i+1)); done; }
mk() { cat > "$TMP/$1.py"; }

echo "== 基线 =="; expect_green "基线"

echo "== D1：远端→本地 判成上传 =="
mk gtmut_d1 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="""        case (.remote, .local):
            return .download(remote: item.path, name: item.name,
                             isDirectory: item.isDirectory)"""
new="""        case (.remote, .local):
            return .upload(local: item.path, name: item.name,
                           isDirectory: item.isDirectory) /* GT_MUTATION_MARK */"""
assert old in s, "D1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_d1.py" "${FILES[0]}"; expect_red "D1 拖放方向断言失效"; rollback

echo "== D2：decode 不还原转义 =="
mk gtmut_d2 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="""        guard let path = ImageEscape.decode(parts[2]),
              let name = ImageEscape.decode(parts[3]) else { return nil }"""
new="""        let path = parts[2]; let name = parts[3] /* GT_MUTATION_MARK */"""
assert old in s, "D2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_d2.py" "${FILES[0]}"; expect_red "D2 转义还原断言失效"; rollback

echo "== D3：完成回调不触发 =="
mk gtmut_d3 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="        callback?(status)"
new="            /* GT_MUTATION_MARK: 不触发 */"
assert old in s, "D3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_d3.py" "${FILES[1]}"; expect_red "D3 完成回调断言失效"; rollback

echo "== 回滚复绿 =="; expect_green "回滚后"
restore_all; trap - INT TERM
echo; echo "mutation-check-dragdrop: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

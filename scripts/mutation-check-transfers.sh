#!/usr/bin/env bash
# 变异验证：证明**全局传输队列**的断言有牙（无网可跑，跑 swift test）。
#
#   T1 parseProgress 丢掉 total（进度总量断言失效）；
#   T2 job 失败折叠成成功（三态折叠）；
#   T3 冲突 skip 被无视（永远覆盖）；
#   T4 pause 不路由到后端。
#
# 纪律照 AGENTS.md：起跑扫残留标记；trap 复原；回滚比对 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"; mkdir -p "$TMP"
SRC="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TransferManager.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0; BACKUP=""

restore_all() {
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（$MARK）——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-muttr-XXXXXX")"; cp "$SRC" "$BACKUP"

run_test() { ( cd "$ROOT/apple/GhostermApp" && timeout 900 swift test --filter TransferManagerTests >/dev/null 2>&1 ); }
expect_green() { if run_test; then echo "  ✓ $1 → green"; PASS=$((PASS+1)); else echo "  ✗ $1 → 期望 green，实际 red"; FAIL=$((FAIL+1)); fi; }
expect_red() { if run_test; then echo "  ✗ $1 → 期望 red，实际 green（断言无牙）"; FAIL=$((FAIL+1)); else echo "  ✓ $1 → red"; PASS=$((PASS+1)); fi; }
mutate() { python3 "$1" "$SRC"; }
rollback() { cp "$BACKUP" "$SRC"
  [ "$(shasum -a 256 "$SRC" | awk '{print $1}')" = "$BASE_SHA" ] || { echo "  ✗ 回滚 sha256 不一致" >&2; FAIL=$((FAIL+1)); }; }
mk() { cat > "$TMP/$1.py"; }

echo "== 基线 =="; expect_green "基线"

echo "== T1：parseProgress 丢 total =="
mk gtmut_t1 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="        return (d, t)"
new="        return (d, 0) /* GT_MUTATION_MARK */"
assert old in s, "T1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_t1.py"; expect_red "T1 进度总量断言失效"; rollback

echo "== T2：失败折叠成成功 =="
mk gtmut_t2 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="            case .failed: jobs[i].status = .failed(ev.message)"
new="            case .failed: jobs[i].status = .done /* GT_MUTATION_MARK */"
assert old in s, "T2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_t2.py"; expect_red "T2 三态折叠"; rollback

echo "== T3：冲突 skip 被无视 =="
mk gtmut_t3 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="        if let c = applyToAll { return c }"
new="        if let c = applyToAll { return c == .rename ? c : .overwrite } /* GT_MUTATION_MARK */"
assert old in s, "T3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_t3.py"; expect_red "T3 冲突 skip 断言失效"; rollback

echo "== T4：pause 不路由到后端 =="
mk gtmut_t4 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="        try? jobs[i].backend?.pause(jobID: jobs[i].coreJob, true)"
new="        /* GT_MUTATION_MARK: 不路由 */"
assert old in s, "T4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_t4.py"; expect_red "T4 暂停路由断言失效"; rollback

echo "== 回滚复绿 =="; expect_green "回滚后"
restore_all; trap - INT TERM
echo; echo "mutation-check-transfers: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

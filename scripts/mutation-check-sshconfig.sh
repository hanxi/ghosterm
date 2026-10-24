#!/usr/bin/env bash
# 变异验证：证明 ssh_config 闸门有牙（非空跑）。
#
# 纪律（照 AGENTS.md）：
#   - 起跑前扫残留变异标记，命中即 exit 2（假基线比跑不起来更坏）；
#   - trap INT TERM 复原；
#   - 回滚后除比对备份，还比对起跑前记录的 sha256。
# 覆盖 A1~A8（求值/无损/支持集/行内注释与 Forward 防护）+ B1~B6（编辑 API
# 写回）共 14 项变异。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
mkdir -p "$TMP"
SRC="$ROOT/core/src/config.c"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

restore_all() {
# 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（$MARK）——工作区不干净，拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mut-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_gate_quiet() { (cd "$ROOT" && bash scripts/gate.sh >/dev/null 2>&1); }

expect_gate() {
  if run_gate_quiet; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then
    echo "  ✓ $2 → 闸门 $actual"; PASS=$((PASS+1))
  else
    echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1))
  fi
}

rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  if [ "$now" != "$BASE_SHA" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1)); return 1
  fi
}

mutate() { # mutate <label> <python-file> <target>
  python3 "$2" "$3"
}

echo "== 基线 =="
expect_gate green "基线"

# ---- A1：无损写回被摘 ----
echo "== A1：to_string 跳过最后一行（无损往返失效） =="
cat > "$TMP/gtmut_a1.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (n->included) continue;\n    size_t l = strlen(n->raw);"
new="    if (n->included) continue;\n    if (i + 1 == cfg->nnodes) continue; /* GT_MUTATION_MARK */\n    size_t l = strlen(n->raw);"
assert old in s, "A1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A1 "$TMP/gtmut_a1.py" "$SRC"
expect_gate red "A1 无损往返失效"
rollback

# ---- A2：Host 块匹配恒真 ----
echo "== A2：block_matches 恒真 =="
cat > "$TMP/gtmut_a2.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  return positive;\n}"
new="  return 1; /* GT_MUTATION_MARK */\n}"
assert old in s, "A2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A2 "$TMP/gtmut_a2.py" "$SRC"
expect_gate red "A2 块匹配失效"
rollback

# ---- A3：SetEnv first-wins 放开 ----
echo "== A3：SetEnv 累积（语义漂移） =="
cat > "$TMP/gtmut_a3.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old='    if (strcmp(n->keyword, "host") == 0) continue;\n    int idx = key_index(n->keyword);'
assert old in s, "A3 anchor missing"
# 把 setenv 从 K_LIST_FIRST 降级为 K_LIST（等价于放开 first-wins）
old2='    {"setenv", K_LIST_FIRST, NULL},'
new2='    {"setenv", K_LIST, NULL}, /* GT_MUTATION_MARK */'
assert old2 in s, "A3 table anchor missing"
open(p,'w').write(s.replace(old2,new2,1))
PY
mutate A3 "$TMP/gtmut_a3.py" "$SRC"
expect_gate red "A3 SetEnv 语义漂移"
rollback

# ---- A4：显式 IdentityFile 也追加默认链 ----
echo "== A4：显式条目也追加默认链 =="
cat > "$TMP/gtmut_a4.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  if (ids->n == 0) {"
new="  if (ids->n >= 0) { /* GT_MUTATION_MARK */"
assert old in s, "A4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A4 "$TMP/gtmut_a4.py" "$SRC"
expect_gate red "A4 显式条目不追加默认链失效"
rollback

# ---- A5：单值 first-wins → last-wins ----
echo "== A5：单值 first-wins 失效 =="
cat > "$TMP/gtmut_a5.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="      case K_SCALAR:\n        if (s->set) break; /* first-wins */\n"
new="      case K_SCALAR:\n        /* GT_MUTATION_MARK */\n"
assert old in s, "A5 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A5 "$TMP/gtmut_a5.py" "$SRC"
expect_gate red "A5 first-wins 失效"
rollback

# ---- A6：支持集漂移（C 表删键，夹具元数据未同步） ----
echo "== A6：支持集漂移 =="
cat > "$TMP/gtmut_a6.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old='    {"escapechar", K_SCALAR, "~"},\n'
assert old in s, "A6 anchor missing"
open(p,'w').write(s.replace(old,"",1))
PY
mutate A6 "$TMP/gtmut_a6.py" "$SRC"
expect_gate red "A6 支持集漂移未拦"
rollback

# ==================================================================
# B 段：ssh_config 编辑 API / 写回闸门（fixtures/writeback.json）
# ==================================================================

# ---- B1：编辑既有行时丢失缩进（无损写回退化） ----
echo "== B1：编辑后丢失既有缩进 =="
cat > "$TMP/gtmut_b1.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  *indent = gt__strndup(is, (size_t)(p - is));"
new="  *indent = gt__strdup(\"\"); /* GT_MUTATION_MARK */"
assert old in s, "B1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate B1 "$TMP/gtmut_b1.py" "$SRC"
expect_gate red "B1 编辑后缩进丢失"
rollback

# ---- B2：累积型退化——一行一条目被挤成一条 ----
echo "== B2：累积型指令被挤成一行 =="
cat > "$TMP/gtmut_b2.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
s2 = s.replace("static bool render_per_line(const char *kw) {",
               "static bool render_per_line(const char *kw) {\n  return false; /* GT_MUTATION_MARK */", 1)
assert s2 != s, "B2 anchor missing"
open(p,'w').write(s2)
PY
mutate B2 "$TMP/gtmut_b2.py" "$SRC"
expect_gate red "B2 累积型被挤成一行"
rollback

# ---- B3：reset 不删旧块（重复块） ----
echo "== B3：reset 未删旧块 =="
cat > "$TMP/gtmut_b3.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (b >= 0) {\n      size_t s, e;\n      block_range(&l, b, &s, &e);\n      if (elist_splice"
new="    if (0 && b >= 0) { /* GT_MUTATION_MARK */\n      size_t s, e;\n      block_range(&l, b, &s, &e);\n      if (elist_splice"
assert old in s, "B3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate B3 "$TMP/gtmut_b3.py" "$SRC"
expect_gate red "B3 reset 未删旧块"
rollback

# ---- B4：关键字原样拼写丢失 ----
echo "== B4：关键字原样拼写丢失 =="
cat > "$TMP/gtmut_b4.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="      if (nfound) split_line(l.v[first].raw, &indent, &kwspell, &sep);"
new="      if (nfound) { split_line(l.v[first].raw, &indent, &kwspell, &sep); free(kwspell); kwspell = gt__strdup(\"XXX\"); } /* GT_MUTATION_MARK */"
assert old in s, "B4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate B4 "$TMP/gtmut_b4.py" "$SRC"
expect_gate red "B4 关键字原样拼写丢失"
rollback

# ---- B5：`=` 分隔符丢失 ----
echo "== B5：= 分隔符丢失 =="
cat > "$TMP/gtmut_b5.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  *sep = gt__strndup(ss, (size_t)(p - ss));"
new="  *sep = gt__strdup(\" \"); /* GT_MUTATION_MARK */"
assert old in s, "B5 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate B5 "$TMP/gtmut_b5.py" "$SRC"
expect_gate red "B5 = 分隔符丢失"
rollback

# ---- B6：`Keyword=value` 不解析（parser 漂移） ----
echo "== B6：Keyword=value 解析漂移 =="
cat > "$TMP/gtmut_b6.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="      while (q < le && !isspace((unsigned char)*q) && *q != '=') q++;"
new="      while (q < le && !isspace((unsigned char)*q)) q++; /* GT_MUTATION_MARK */"
assert old in s, "B6 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate B6 "$TMP/gtmut_b6.py" "$SRC"
expect_gate red "B6 Keyword=value 未切开"
rollback

# ---- A7：行内注释剥离被摘（注释当 token 进 args → 归一化被污染） ----
echo "== A7：行内注释剥离被摘 =="
cat > "$TMP/gtmut_a7.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (*p == '#') break;"
new="    /* GT_MUTATION_MARK */"
assert old in s, "A7 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A7 "$TMP/gtmut_a7.py" "$SRC"
expect_gate red "A7 行内注释进 args"
rollback

# ---- A8：Forward 负差值防护路径破坏 ----
# 原始缺陷是 memchr 负差值越界（SIGBUS，堆布局相关、不确定崩）；变异用
# 「防护路径丢值」这一确定性破坏证明断言对该路径敏感（丢值必红；越界则
# 崩或不崩都过不了全量闸门）。
echo "== A8：Forward 负差值防护路径破坏 =="
cat > "$TMP/gtmut_a8.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  if (hend <= hstart) return joined;"
new="  if (hend <= hstart) return NULL; /* GT_MUTATION_MARK */"
assert old in s, "A8 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate A8 "$TMP/gtmut_a8.py" "$SRC"
expect_gate red "A8 防护路径丢值"
rollback

echo "== 回滚复绿 =="
expect_gate green "回滚后"

restore_all
trap - INT TERM
echo
echo "mutation-check-sshconfig: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

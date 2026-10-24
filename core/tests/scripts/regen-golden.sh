#!/usr/bin/env bash
# 用本机真 `ssh -G` 再生 ssh_config 语义锚点。
#
#   产物 1：fixtures/golden.effective.tsv —— 对拍用（host<TAB>key<TAB>value，多值多行）
#   产物 2：fixtures/golden.json          —— 人读/工具读的完整 dump
#
# ⚠️ 必须在**钉版本环境**执行，并保证系统 /etc/ssh/ssh_config 干净
#    （`ssh -G` 同时读系统级与用户级配置）。换环境再生＝换语义基线，
#    提交时必须显式标注 BREAKING。详见 core/tests/README.md。
#
# 支持集（哪些键纳入对拍）的**单一事实来源是 core/src/config.c 的 k_keys 表**，
# 本脚本从那里提取；表改了就必须重跑本脚本，否则 test_config 会因元数据过期而红。
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$(cd "$HERE/../.." && pwd)"
FIX="$CORE/tests/fixtures"
CONF="$FIX/golden.conf"
TSV="$FIX/golden.effective.tsv"
JSON="$FIX/golden.json"
CFG_C="$CORE/src/config.c"

[ -f "$CONF" ] || { echo "regen-golden: 缺少 $CONF" >&2; exit 2; }
command -v ssh >/dev/null || { echo "regen-golden: 需要 ssh" >&2; exit 2; }

# 1) 从 C 表提取支持集（单一事实来源）
KEYS="$(sed -n '/^static const keydef_t k_keys\[\]/,/^};/p' "$CFG_C" \
        | sed -nE 's/^[[:space:]]*\{[[:space:]]*"([a-z0-9]+)".*/\1/p')"
[ -n "$KEYS" ] || { echo "regen-golden: 未能从 $CFG_C 提取 k_keys" >&2; exit 2; }

HOSTS="web api db.prod secret jumpchain other"
DEFAULT_USER="$(id -un)"

# 2a) 匿名化：锚点会随仓库公开，绝不能带再生机器的登录名与家目录。
#     `ssh -G` 的默认值里这两处是环境相关（无 User 时的回落用户、`~` 展开后的
#     known_hosts 路径），统一映射成中性 token 再落盘；对拍两侧读同一份夹具，
#     语义不变。可用 GOLDEN_NEUTRAL_USER / GOLDEN_NEUTRAL_HOME 覆盖。
NEUTRAL_USER="${GOLDEN_NEUTRAL_USER:-goldenuser}"
NEUTRAL_HOME="${GOLDEN_NEUTRAL_HOME:-/home/goldenuser}"
export GOLDEN_NEUTRAL_USER="$NEUTRAL_USER" GOLDEN_NEUTRAL_HOME="$NEUTRAL_HOME"
REAL_HOME="${HOME:-}"

# 2b) 本机默认 IdentityFile 候选链（无配置时 ssh 自己会用的那串）
FALLBACK="$(ssh -G -F /dev/null probe.invalid 2>/dev/null | awk '$1=="identityfile"{ $1=""; sub(/^ /,""); print }')"

# ---- 写 TSV ----
{
  echo "# generated_by	$(ssh -V 2>&1)"
  echo "# default_user	$NEUTRAL_USER"
  echo "# anchor_note	ssh -G reads system /etc/ssh/ssh_config too; anchor env must be clean"
  echo "# anchor_note	values are anonymized: login name and home dir never reach the fixture"
  while IFS= read -r k; do [ -n "$k" ] && echo "# supported_key	$k"; done <<< "$KEYS"
  while IFS= read -r f; do [ -n "$f" ] && echo "# identity_fallback	$f"; done <<< "$FALLBACK"

  for h in $HOSTS; do
    dump="$(ssh -G -F "$CONF" "$h" 2>/dev/null)"
    while IFS= read -r k; do
      [ -n "$k" ] || continue
      printf '%s\n' "$dump" | awk -v h="$h" -v k="$k" -v usr="$DEFAULT_USER" -v nusr="$NEUTRAL_USER" \
          -v home="$REAL_HOME" -v nhome="$NEUTRAL_HOME" \
        'function lit(s, from, to,   i, out) {
           out = s; while (from != "" && (i = index(out, from)) > 0)
             out = substr(out, 1, i - 1) to substr(out, i + length(from));
           return out }
         $1==k { $1=""; sub(/^ /,""); v=$0;
                 if (k=="user" && v==usr) v=nusr;
                 v=lit(v, home, nhome);
                 printf "%s\t%s\t%s\n", h, k, v }'
    done <<< "$KEYS"
  done
} > "$TSV"

# ---- 写 JSON（完整 dump，人读） ----
python3 - "$CONF" "$JSON" $HOSTS <<'PY' 2>/dev/null || echo "regen-golden: 跳过 JSON（python 不可用）" >&2
import json, os, subprocess, sys
conf, out = sys.argv[1], sys.argv[2]
hosts = sys.argv[3:]
neutral_user = os.environ.get("GOLDEN_NEUTRAL_USER", "goldenuser")
neutral_home = os.environ.get("GOLDEN_NEUTRAL_HOME", "/home/goldenuser")
real_home = os.environ.get("HOME", "")
real_user = subprocess.run(["id", "-un"], capture_output=True, text=True).stdout.strip()
def anon(v, key):
    if key == "user" and v == real_user: v = neutral_user
    if real_home: v = v.replace(real_home, neutral_home)
    return v
ids = subprocess.run(["ssh","-G","-F","/dev/null","probe.invalid"],
                     capture_output=True, text=True).stdout.splitlines()
fb = [anon(l.split(None,1)[1], "identityfile") for l in ids if l.startswith("identityfile ")]
ver = subprocess.run(["ssh","-V"], capture_output=True, text=True).stderr.strip()
data = {"generated_by": ver, "identity_fallback": fb, "hosts": {}}
for h in hosts:
    dump = subprocess.run(["ssh","-G","-F",conf,h],
                          capture_output=True, text=True).stdout.splitlines()
    m = {}
    for line in dump:
        parts = line.split(None, 1)
        if not parts: continue
        k = parts[0]; v = parts[1] if len(parts) > 1 else ""
        m.setdefault(k, []).append(anon(v, k))
    data["hosts"][h] = m
json.dump(data, open(out,"w"), indent=2, ensure_ascii=False)
print("written:", out)
PY

# ---- 匿名化守卫：真实登录名/家目录一旦漏进夹具就当场失败，不产出脏锚点 ----
leak=0
if [ -n "$REAL_HOME" ] && grep -qF -- "$REAL_HOME" "$TSV" "$JSON" 2>/dev/null; then leak=1; fi
if grep -qE "^# default_user[[:space:]]+$DEFAULT_USER\$" "$TSV" 2>/dev/null; then leak=1; fi
if [ "$leak" -ne 0 ]; then
  echo "regen-golden: 夹具里残留本机登录名或家目录，拒绝落盘" >&2
  exit 1
fi

echo "written: $TSV"
echo "  keys: $(printf '%s\n' "$KEYS" | grep -c . | tr -d ' ')  hosts: $(echo $HOSTS | wc -w | tr -d ' ')"

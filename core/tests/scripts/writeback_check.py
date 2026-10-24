#!/usr/bin/env python3
"""ssh_config 编辑 API 的写回对拍（M1 闸门）。

单一事实来源：core/tests/fixtures/writeback.json（9 用例，来自归档实现
并用真 `ssh -G rc=0` 验证过）。本脚本把每个用例的稀疏编辑翻译成 gtb 的
分组 spec，跑 C 编辑 API，再逐字节比对期望输出。

用法：writeback_check.py <gtb> <fixtures-dir>
"""
import json, os, subprocess, sys, tempfile

FIELD = {
    "host_name": "HostName",
    "user": "User",
    "port": "Port",
    "identity_file": "IdentityFile",
    "local_forward": "LocalForward",
    "remote_forward": "RemoteForward",
    "set_env": "SetEnv",
    "proxy_jump": "ProxyJump",
    "proxy_command": "ProxyCommand",
    "request_tty": "RequestTTY",
    "forward_agent": "ForwardAgent",
    "compression": "Compression",
    "server_alive_interval": "ServerAliveInterval",
}


def spec_for(case):
    """把 UI 侧的 case 翻译成 (别名, reset, spec 文本)。"""
    edit = case["edit"]
    lines = []

    def emit(kw, value):
        if value is None or (isinstance(value, list) and len(value) == 0):
            lines.append("KW\t%s" % kw)
            lines.append("DEL")
            return
        vals = value if isinstance(value, list) else [value]
        lines.append("KW\t%s" % kw)
        for v in vals:
            lines.append("V\t%s" % v)

    for key, value in edit.items():
        if key in ("alias", "tags"):
            continue
        if key == "extra":
            for kw, vals in value.items():
                emit(kw, vals)
        else:
            emit(FIELD[key], value)
    return edit["alias"], case.get("reset", False), "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 3:
        print("usage: writeback_check.py <gtb> <fixtures-dir>", file=sys.stderr)
        return 2
    tool, fixdir = sys.argv[1], sys.argv[2]
    spec = json.load(open(os.path.join(fixdir, "writeback.json")))
    sample = spec["sample"]
    root = os.path.abspath(os.path.join(fixdir, "..", "..", ".."))
    tmp = os.path.join(root, "tmp")
    os.makedirs(tmp, exist_ok=True)

    fails = 0
    with tempfile.TemporaryDirectory(dir=tmp) as work:
        conf = os.path.join(work, "sample.conf")
        open(conf, "w").write(sample)
        for case in spec["cases"]:
            alias, reset, text = spec_for(case)
            sp = os.path.join(work, "spec.txt")
            open(sp, "w").write(text)
            out = os.path.join(work, "out.conf")
            r = subprocess.run([tool, "edit", conf, out, alias,
                                "1" if reset else "0", sp],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("FAIL %s: gtb rc=%d %s" % (case["name"], r.returncode, r.stderr.strip()))
                fails += 1
                continue
            got = open(out).read()
            if got == case["output"]:
                print("ok   %s" % case["name"])
            else:
                print("FAIL %s: 输出与夹具不符" % case["name"])
                fails += 1
                if os.environ.get("WRITEBACK_VERBOSE"):
                    import difflib
                    sys.stdout.writelines(difflib.unified_diff(
                        case["output"].splitlines(True), got.splitlines(True),
                        "expected", "got"))
    # ---- 补充断言：触及「既有行用 = 分隔」时必须保留 = ----
    # （golden 的 restyle 用例只触及空格分隔的 Port，不覆盖此点，故单列。）
    fails += supplement_equals_preserved(tool, sample, tmp)

    print("writeback_check: %d cases, %d failed" % (len(spec["cases"]) + 1, fails))
    return 1 if fails else 0


def supplement_equals_preserved(tool, sample, tmp):
    """bastion 的 IdentityFile 用 `=` 分隔；改它必须仍是 `=`，见头号不变量。"""
    conf = os.path.join(tmp, "eq-src.conf")
    specp = os.path.join(tmp, "eq-spec.txt")
    outp = os.path.join(tmp, "eq-out.conf")
    open(conf, "w").write(sample)
    open(specp, "w").write("KW\tIdentityFile\nV\t~/.ssh/new_key\n")
    r = subprocess.run([tool, "edit", conf, outp, "bastion", "0", specp],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL equals_preserved: gtb rc=%d %s" % (r.returncode, r.stderr.strip()))
        return 1
    got = open(outp).read()
    if "  IdentityFile=~/.ssh/new_key\n" in got:
        print("ok   retains_equals_separator")
        return 0
    print("FAIL retains_equals_separator: `=` 未保留")
    if os.environ.get("WRITEBACK_VERBOSE"):
        for line in got.splitlines():
            if "IdentityFile" in line:
                print("   got: %r" % line)
    return 1


if __name__ == "__main__":
    sys.exit(main())

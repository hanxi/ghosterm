#!/usr/bin/env python3
"""Linux 侧会话运行时构建器：打同一套补丁的 libghostty-vt + 完整 include 树 + sessiond bundle。

macOS 的 .app 必须内嵌 Linux 会话安装包，而 `scripts/stage-session-runtime.py` 要求 bundle
的 session contract 与 mac 端**逐位一致**。契约是 `daemon/CMakeLists.txt` 对「include 根下
全部 `*.h` + `patches/libghostty/8159838/*.patch` + schema 源文件」逐一取 sha256 拼出来的，
与二进制、构建路径无关。于是 Linux 侧只有两条铁规矩：

1. 引擎副本与补丁**全集、全顺序**必须和 mac 探针链同源（复用 `FULL['prepare']` +
   `probe-session-worker-surface.py` 的 `PATCHES`，不在本脚本里另立一份清单）；
2. `--include` 必须指向 `<engine>/include`（顶层 `ghostty.h` 在内共 36 个头），**不能**用
   `zig build` 的 `zig-out/include` —— 上游 `installHeadersDirectory` 只装 `include/ghostty`
   子目录（35 个头），少一个头契约就变，mac 打包会直接报 `Linux bundle identity mismatch`。

`--dry-run` 只走「副本 + 补丁 + 头清单」，不出 `.so`、不出包：给开发机（含 macOS）做契约
预检用，与已验证的 mac SDK 头树逐字节比对。真实构建要求 Linux aarch64/x86_64。
"""
import argparse
import hashlib
import json
import os
import platform
import re
import runpy
import shutil
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "81598385d608dda4946093d6d6a234d204a2f225"
WORK = ROOT / "tmp/vt-linux"
SONAME = "libghostty-vt.so.0"
VERSIONED = re.compile(r"^libghostty-vt\.so\.\d+\.\d+\.\d+$")
MARK = re.compile(r"GT_[A-Z0-9_]*MUTATION_MARK")
PATCHDIR = ROOT / "patches/libghostty/8159838"


def checked(command, *, timeout=600, cwd=None, env=None, stdout=None):
    """跑一条外部命令；失败/超时都要杀掉整个进程组再报错，不留孤儿编译器。"""
    process = subprocess.Popen([str(x) for x in command], cwd=cwd, env=env, stdout=stdout,
                               stderr=None if stdout is None else subprocess.STDOUT,
                               text=stdout is not None, start_new_session=True)
    try:
        if process.wait(timeout=timeout) != 0:
            raise RuntimeError("命令失败（退出码非 0）: " + " ".join(map(str, command[:3])))
    except BaseException:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        raise
    return process


def contract_inputs():
    """契约输入 = CMakeLists 里被哈希的那两批文件；schema 清单从 CMakeLists 现取，不复制常量。"""
    text = (ROOT / "daemon/CMakeLists.txt").read_text()
    block = re.search(r"set\(session_schemas\s+(.*?)\)\s*\n", text, re.S)
    if not block:
        raise RuntimeError("未能从 daemon/CMakeLists.txt 提取 session_schemas")
    return sorted(PATCHDIR.rglob("*.patch")), [ROOT / token for token in block.group(1).split()]


def guard_mutation_marks():
    """起跑前扫变异标记：补丁或 schema 里残留着变异 = 契约输入已被改坏，直接拒绝。"""
    patches, schemas = contract_inputs()
    hits = [str(path) for path in (*patches, *schemas, ROOT / "scripts/prepare-session-io.py")
            if path.is_file() and MARK.search(path.read_text(errors="replace"))]
    if hits:
        print("契约输入里残留变异标记: " + ", ".join(hits), file=sys.stderr)
        raise SystemExit(2)


def load_suite():
    """补丁全集/顺序的单一事实来源：与 mac 探针链同一个 runpy 入口。"""
    suite = runpy.run_path(str(ROOT / "scripts/probe-session-worker-surface.py"))
    patches = [str(patch) for patch in suite["PATCHES"]]
    if not patches:
        raise RuntimeError("探针套件未导出 PATCHES")
    return suite["FULL"], patches


def stage_engine():
    """vendor 副本 → prepare-session-io → 补丁全集，逐片先 --check 再 apply。"""
    full, patches = load_suite()
    vendor = ROOT / "vendor/ghostty"
    checked(["git", "-C", vendor, "diff", "--quiet", "HEAD", "--"])
    commit = subprocess.check_output(["git", "-C", vendor, "rev-parse", "HEAD"], text=True).strip()
    if commit != PIN:
        raise RuntimeError(f"engine pin mismatch: {commit} != {PIN}")
    for stale in ("engine", "engine.tar"):
        path = WORK / stale
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists() or path.is_symlink():
            path.unlink()
    WORK.mkdir(parents=True, exist_ok=True)
    engine = full["prepare"](WORK)
    for patch in patches:
        checked(["git", "-C", engine, "apply", "--check", patch])
        checked(["git", "-C", engine, "apply", patch])
    if not (engine / "include/ghostty/vt.h").is_file():
        raise RuntimeError("补丁后的引擎缺少公开 VT 头: include/ghostty/vt.h")
    return engine, patches


def header_map(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*.h"))}


def build_vt(engine):
    """与 nix/libghostty-vt.nix 同方子的 ReleaseFast 共享库；-Dcpu=baseline 保证下发到他机可跑。"""
    zig = os.environ.get("ZIG") or shutil.which("zig")
    if not zig:
        raise RuntimeError("需要 zig 0.16.0 在 PATH（或设 ZIG 环境变量）")
    env = dict(os.environ, ZIG_LOCAL_CACHE_DIR=str(ROOT / "tmp/zig-cache-vt-linux"),
               ZIG_GLOBAL_CACHE_DIR=str(ROOT / "tmp/zig-global"))
    log = WORK / "build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as stream:
        # ReleaseFast 全量构建在慢 runner 上远超探针脚本的 600s，这里单独放宽。
        checked([zig, "build", "-Demit-lib-vt=true", "-Demit-xcframework=false",
                 "-Dapp-runtime=none", "-Dcpu=baseline", "-Doptimize=ReleaseFast",
                 "-p", WORK / "out"], timeout=1800, cwd=engine, env=env, stdout=stream)
    return locate_shared(WORK / "out/lib")


def locate_shared(libdir):
    """找到真正的 .so.<maj>.<min>.<pat>，验 SONAME，并保证 SONAME 名的入口存在。"""
    real = sorted((path for path in libdir.iterdir() if VERSIONED.match(path.name)))
    if len(real) != 1:
        raise RuntimeError(f"期望恰好一个带版本号的共享库，实际 {len(real)}: {libdir}")
    binary = real[0]
    dynamic = subprocess.check_output(["readelf", "-d", binary], text=True)
    soname = re.search(r"Library soname: \[([^]]+)\]", dynamic)
    if not soname or soname.group(1) != SONAME:
        raise RuntimeError(f"SONAME 不是 {SONAME}: {soname.group(1) if soname else '缺失'}")
    alias = libdir / SONAME
    if not alias.exists():
        alias.symlink_to(binary.name)
    if alias.read_bytes() != binary.read_bytes():
        raise RuntimeError(f"{SONAME} 与带版本号的实体库内容不一致")
    return binary


def write_evidence(engine, patches, binary, path):
    include = engine / "include"
    evidence = {
        "pin": PIN,
        "arch": platform.machine() if binary else None,
        "zig": "0.16.0",
        "mode": "build" if binary else "dry-run",
        "patches": [{"name": str(Path(patch).relative_to(ROOT)),
                     "sha256": hashlib.sha256(Path(patch).read_bytes()).hexdigest()} for patch in patches],
        "include_root": "engine/include",
        "headers": header_map(include),
        "library": None if binary is None else {"path": str(binary.relative_to(ROOT)),
                                                "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()},
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    return evidence


def cross_check_mac(engine, reference=None):
    """dry-run 的牙：Linux 侧要喂给 CMake 的头树，必须与已验证的 mac SDK 头树逐字节相同。"""
    reference = Path(reference) if reference else ROOT / "build/GhosttyKit.xcframework/macos-arm64/Headers"
    if not reference.is_dir():
        raise RuntimeError("缺少已验证的 macOS SDK 头树作参照：先跑 scripts/build-session-engine.py")
    local, remote = header_map(engine / "include"), header_map(reference)
    if local != remote:
        drift = sorted(set(local) ^ set(remote)) or [name for name in local if local[name] != remote[name]]
        raise RuntimeError("头集合与 mac SDK 不一致（契约必红）: " + ", ".join(drift[:5]))
    return len(local)


def package(engine, binary):
    """调既有打包脚本出 bundle；本脚本不复制任何 bundle 校验逻辑。"""
    result = subprocess.run([sys.executable, ROOT / "scripts/package-sessiond-linux.py",
                             "--include", engine / "include", "--engine", binary.parent / SONAME,
                             "--build-dir", WORK / "daemon", "--out", WORK / "bundles"],
                            capture_output=True, text=True, check=False)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode:
        raise RuntimeError("package-sessiond-linux.py 失败")
    bundle = Path(result.stdout.strip().splitlines()[-1])
    if not (bundle / "manifest.tsv").is_file():
        raise RuntimeError("打包脚本没交出 bundle 目录: " + str(bundle))
    return bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true",
                        help="只做「副本 + 补丁 + 头清单」，不构建 .so、不出包（可在 macOS 上跑）")
    parser.add_argument("--evidence", type=Path, default=WORK / "vt-evidence.json")
    args = parser.parse_args()
    if not args.dry_run and (platform.system() != "Linux" or platform.machine() not in ("aarch64", "x86_64")):
        raise RuntimeError("真实构建需要 Linux aarch64/x86_64 builder；本机请改用 --dry-run")
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    os.umask(0o077)
    guard_mutation_marks()
    engine, patches = stage_engine()
    binary = None if args.dry_run else build_vt(engine)
    evidence = write_evidence(engine, patches, binary, args.evidence)
    line = f"引擎头 {len(evidence['headers'])} 个（<engine>/include）: {args.evidence}"
    if args.dry_run:
        if sys.platform == "darwin":
            line += f"；与 mac SDK 头树逐字节一致（{cross_check_mac(engine)} 个）"
        print("PASS dry-run 契约预检 " + line, flush=True)
    else:
        bundle = package(engine, binary)
        print(f"PASS libghostty-vt {binary.name} + bundle {bundle}；" + line, flush=True)
        print(bundle)
    return 0


if __name__ == "__main__":
    sys.exit(main())

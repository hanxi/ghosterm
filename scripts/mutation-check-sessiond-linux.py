#!/usr/bin/env python3
"""变异验证：Linux 会话安装包的四条断言都得有牙（需要 Linux builder）。

护住的是「mac 与 Linux 两侧 session contract 逐位一致」这条链条：契约由
`daemon/CMakeLists.txt` 对「include 根下全部 `*.h` + `patches/libghostty/8159838/*.patch`
+ schema 源文件」取 sha256 拼出，`scripts/stage-session-runtime.py` 拿它当唯一身份。
本脚本**复用** `scripts/build-session-vt-linux.py` 的 staging/打包实现（import 同一份
代码），只做「把某个输入改坏 → 断言变红 → 回滚」的编排，绝不复制一份构建逻辑。

四条变异与它们的**指向性对照**：

  M1 少打一片补丁 → 契约 ≠ 基线。契约只哈希「头树 + 补丁文件本身 + schema」，
     所以只改 `.zig` 的补丁摘掉了契约**看不出来**——这是契约的真实边界，M1 因此挑
     **真改到 include/ 的最后一片**，并在动手前先证明头树确实变了（防止假变异糊过去）。
  M2 vendor pin 被改 → 构建器当场拒绝（engine pin mismatch），不是默默照抄。
  M3 把 `.a` 冒充 `--engine` → 新增的 DT_NEEDED **下界**断言红；
     对照：静态链接后的 NEEDED ⊆ 基线（动态链接）的 NEEDED，旧的 `allowed` 上界白名单
     对它完全无感 ⇒ 红的是新牙，不是构建碰巧挂了。
  M4 改一个头的一字节 → 契约 ≠ 基线；对照 `scripts/mutation-check-public-headers.py`
     仍绿（本轮没动 `core/`）。

非 Linux 主机：显式打印 SKIP 并 `exit 0`（口径同「无 swift / 无 Pillow」那几条），
CI 的 `linux-bundle` job（`gates=full`）必须真跑到，那边断言输出不含 `SKIP`。
"""
import argparse
import hashlib
import importlib.util
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "tmp/vt-linux"                      # 基线产物：CI 上一条 step 刚产出
MUT = ROOT / "tmp/vt-linux-mutation"
MARK = "GT_SESSIOND_LINUX_MUTATION_MARK"
HEADER = "ghostty/vt.h"
PATCHDIR = ROOT / "patches/libghostty/8159838"
SCANNED = (ROOT / "scripts/build-session-vt-linux.py", ROOT / "scripts/package-sessiond-linux.py",
           ROOT / "scripts/probe-session-worker-surface.py", ROOT / "scripts/probe-session-surface.py",
           ROOT / "daemon/CMakeLists.txt", PATCHDIR, WORK / "engine/include")
ORIGINALS: dict[Path, str] = {}


class CheckFailure(RuntimeError):
    """某条变异没能把目标断言变红 ⇒ 该闸门没牙。"""


def load_builder():
    """被验对象与真构建共用同一份实现：变异脚本只 import，不另写 staging/打包。"""
    spec = importlib.util.spec_from_file_location("gt_session_vt_linux",
                                                 ROOT / "scripts/build-session-vt-linux.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record(*paths):
    """记下变异前的**字节**与 sha：回滚要能真的复原，而不是只喊「没复原」。"""
    for path in paths:
        path = Path(path)
        if path not in ORIGINALS:
            ORIGINALS[path] = digest(path)
            path.with_suffix(path.suffix + ".pre-mutation").write_bytes(path.read_bytes())


def restore():
    """回滚逐字节确认——sha 不匹配就是「变异没清干净」，绝不能留给下一轮。"""
    bad = []
    for path, before in ORIGINALS.items():
        backup = path.with_suffix(path.suffix + ".pre-mutation")
        if path.is_file() and digest(path) != before:
            if backup.is_file() and digest(backup) == before:
                path.write_bytes(backup.read_bytes())
            if digest(path) != before:
                bad.append(str(path))
        backup.unlink(missing_ok=True)
    ORIGINALS.clear()
    if bad:
        raise RuntimeError("变异未回滚: " + ", ".join(bad))


def interrupted(signum, _frame):
    restore()
    signal.signal(signum, signal.SIG_DFL)
    os.kill(os.getpid(), signum)


def needed_libraries(binary):
    dynamic = subprocess.check_output(["readelf", "-d", binary], text=True)
    return set(re.findall(r"Shared library: \[([^]]+)\]", dynamic))


def contract(bundle):
    """bundle 身份 = manifest.tsv 头行的第 6 字段（contract）与第 4 字段（min_glibc）。"""
    header = (Path(bundle) / "manifest.tsv").read_text().splitlines()[0].split("\t")
    if len(header) < 6:
        raise CheckFailure("manifest.tsv 头行不足 6 字段: " + str(bundle))
    return header[5], header[3]


def header_touching_patches(patches):
    """补丁里真改到公开头树的那些——M1 的合法靶子。"""
    return [patch for patch in patches
            if any(line.startswith("+++ b/include/") for line in Path(patch).read_text(errors="replace").splitlines())]


def m1_drop_patch(builder, shared, baseline_contract):
    patch = header_touching_patches(builder.load_suite()[1])[-1]
    work = MUT / "m1"
    work.mkdir(parents=True, exist_ok=True)
    engine, _ = builder.stage_engine(work)
    builder.checked(["git", "-C", engine, "apply", "--reverse", patch])
    mutated, baseline = builder.header_map(engine / "include"), builder.header_map(WORK / "engine" / "include")
    drift = sorted(set(mutated) ^ set(baseline)) or [name for name in mutated if mutated[name] != baseline[name]]
    if not drift:
        raise CheckFailure(f"M1 目标补丁 {Path(patch).name} 没改到头树：换一片真的，别用假变异糊过去")
    bundle = builder.package(engine / "include", shared, work)
    value, _ = contract(bundle)
    if value == baseline_contract:
        raise CheckFailure("少打一片补丁后契约仍等于基线（断言没牙）")
    return (f"少打 {Path(patch).name}（动了 {drift[0]}）→ 契约 {value[:12]}… ≠ 基线 {baseline_contract[:12]}…")


def m2_pin_guard(builder, work_dir):
    original, builder.PIN = builder.PIN, "0" * 40
    try:
        builder.stage_engine(work_dir)
    except RuntimeError as failure:
        if "engine pin mismatch" not in str(failure):
            raise CheckFailure("构建器失败但不是 pin mismatch: " + str(failure)[-300:])
        return "vendor pin 被改 → 构建器当场拒绝（engine pin mismatch）"
    finally:
        builder.PIN = original
    raise CheckFailure("篡改 vendor pin 后构建器仍照常出货（pin 闸门没牙）")


def m3_static_engine_smuggling():
    archive = WORK / "out/lib/libghostty-vt.a"
    if not archive.is_file():
        raise CheckFailure("M3 需要 out/lib/libghostty-vt.a 作为冒充材料")
    work = MUT / "m3"
    work.mkdir(parents=True, exist_ok=True)
    # 直接调打包脚本而不是构建器的 package()：构建器只会递 SONAME 名的共享库，
    # 而 M3 要的就是把 .a 塞进去；--no-strip 是让失败落在断言上、而不是落在 strip 上。
    result = subprocess.run([sys.executable, ROOT / "scripts/package-sessiond-linux.py",
                             "--include", WORK / "engine/include", "--engine", archive,
                             "--build-dir", work / "daemon", "--out", work / "bundles",
                             "--no-strip"], capture_output=True, text=True, check=False)
    static_binary = work / "daemon/ghosterm-sessiond"
    if result.returncode == 0:
        raise CheckFailure("把 .a 当 --engine 竟然出了包（下界断言没牙）")
    if "没有动态链接 libghostty-vt.so.0" not in result.stderr:
        raise CheckFailure("M3 失败但不是下界断言（可能是真构建挂了）: "
                           + (result.stderr or result.stdout).strip()[-300:])
    if not static_binary.is_file():
        raise CheckFailure("M3 没留下静态链接的 sessiond，无法做指向性对照: " + str(static_binary))
    baseline_shared = needed_libraries(WORK / "daemon/ghosterm-sessiond")
    smuggled = needed_libraries(static_binary)
    if not smuggled <= baseline_shared or "libghostty-vt.so.0" not in baseline_shared:
        raise CheckFailure("指向性对照失效：静态版 NEEDED 不是基线的子集，旧的上界白名单也会拦下它")
    return (f"静态库冒充 --engine → DT_NEEDED 下界断言红"
            f"（对照：NEEDED {sorted(smuggled)} ⊆ 基线，旧白名单仍绿）")


def m4_header_byte(builder, shared, baseline_contract):
    """在基线头树的**副本**上改一字节：基线产物不能被变异污染。"""
    source = WORK / "engine/include"
    if not (source / HEADER).is_file():
        raise CheckFailure("缺基线引擎头树：先跑 scripts/build-session-vt-linux.py")
    work = MUT / "m4"
    work.mkdir(parents=True, exist_ok=True)
    include = work / "include"
    shutil.copytree(source, include)
    header = include / HEADER
    record(header)
    header.write_bytes(header.read_bytes() + b"\n// " + MARK.encode() + b"\n")
    if digest(header) == digest(source / HEADER):
        raise CheckFailure("M4 的变异没落到字节上（假变异）")
    bundle = builder.package(include, shared, work)
    value, _ = contract(bundle)
    restore()
    if value == baseline_contract:
        raise CheckFailure("改头的一字节后契约仍等于基线（断言没牙）")
    control = subprocess.run([sys.executable, ROOT / "scripts/mutation-check-public-headers.py"],
                             capture_output=True, text=True, check=False)
    if control.returncode:
        raise CheckFailure("对照失效：没动 core 而公开头闸门也红了 → "
                           + (control.stderr or control.stdout).strip()[-300:])
    return f"改一字节 → 契约 {value[:12]}… ≠ 基线 {baseline_contract[:12]}…（core 公开头对照仍绿）"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--case", action="append", choices=["M1", "M2", "M3", "M4"],
                        help="只跑指定变异（可重复）；默认全跑")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "x86_64"):
        print("SKIP 需要 Linux aarch64/x86_64 builder（CI gates=full 的 linux-bundle job 必跑）")
        return 0
    for target in SCANNED:
        sources = [target] if target.is_file() else sorted(p for p in target.rglob("*") if p.is_file()) \
            if target.is_dir() else []
        if any(MARK in path.read_bytes().decode(errors="replace") for path in sources):
            print("被验脚本/契约输入里残留变异标记: "
                  + ", ".join(str(p) for p in sources if MARK in p.read_bytes().decode(errors="replace")),
                  file=sys.stderr)
            return 2
    (ROOT / "tmp").mkdir(exist_ok=True)
    os.environ["TMPDIR"] = str(ROOT / "tmp")
    os.umask(0o077)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    shutil.rmtree(MUT, ignore_errors=True)
    MUT.mkdir(parents=True)
    builder = load_builder()
    shared = builder.locate_shared(WORK / "out/lib")
    bundles = sorted((WORK / "bundles").glob("*/manifest.tsv"))
    if not (WORK / "engine/include" / HEADER).is_file() or not bundles:
        raise RuntimeError("缺基线产物：先跑 python3 scripts/build-session-vt-linux.py")
    baseline_contract, min_glibc = contract(bundles[0].parent)
    headers = builder.header_map(WORK / "engine/include")
    print(f"基线契约 {baseline_contract[:16]}…（min_glibc {min_glibc}，引擎头 {len(headers)} 个）", flush=True)
    cases = {"M1": lambda: m1_drop_patch(builder, shared, baseline_contract),
             "M2": lambda: m2_pin_guard(builder, MUT / "m2"),
             "M3": lambda: m3_static_engine_smuggling(),
             "M4": lambda: m4_header_byte(builder, shared, baseline_contract)}
    failures = []
    for name in args.case or sorted(cases):
        try:
            print(f"PASS {name}: {cases[name]()}", flush=True)
        except (CheckFailure, RuntimeError) as failure:
            failures.append(f"{name}: {failure}")
            print(f"FAIL {name}: {failure}", flush=True)
        try:
            restore()
        except RuntimeError as failure:
            failures.append(str(failure))
            print(f"FAIL 回滚: {failure}", flush=True)
    shutil.rmtree(MUT, ignore_errors=True)
    if failures:
        print("MUTATION-CHECK-SESSIOND-LINUX FAIL\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print("MUTATION-CHECK-SESSIOND-LINUX PASS: 契约链条的四条断言都有牙", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

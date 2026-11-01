#!/usr/bin/env python3
"""macOS Kitty 图形手工验证：真实 Metal 像素、协议回包、隔离变异与恢复。

运行 python3 scripts/test-kitty-macos.py；先用 scripts/build-ghostty.sh 构建引擎。
日志和截图保留在工程 tmp/kitty-macos-*；不覆盖完整 App、PTY、SSH 或 tmux。
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "apple/GhostermTerminal"
PROBE = ROOT / "scripts/probes/KittyGraphics.swift"
SURFACE = Path("Sources/GhostermTerminal/TerminalSurface.swift")
CONTROL = "independent text reaches grid"
TARGETS = (
    "Kitty graphics query acknowledged",
    "chunked PNG transmit/display acknowledged",
    "both PNG color regions rendered by Metal",
    "deleting image removes its pixels",
    "invalid PNG rejected explicitly",
    "animation frame accepted",
    "animation frame selection changes rendered pixels",
    "automatic animation playback renders both frames",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, env, log, timeout):
    with log.open("w") as output:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=timeout)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("需要 Apple Silicon macOS 和可用的原生窗口/Metal 环境")
    sdk = ROOT / "build/GhosttyKit.xcframework"
    archive = sdk / "macos-arm64/libghostty-internal.a"
    receipt = json.loads((sdk / "terminal-engine.json").read_text())
    pin = subprocess.check_output(
        ["git", "-C", str(ROOT / "vendor/ghostty"), "rev-parse", "HEAD"], text=True).strip()
    if receipt["engine"] != pin or receipt["archive_sha256"] != digest(archive):
        raise RuntimeError("引擎收据与当前 pin/archive 不一致，请重建引擎")
    for path, expected in receipt["patches"].items():
        if digest(ROOT / path) != expected:
            raise RuntimeError(f"引擎补丁已变化，请重建：{path}")

    (ROOT / "tmp").mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="kitty-macos-", dir=ROOT / "tmp"))
    print(f"日志和截图：{work}", flush=True)
    original = (PACKAGE / SURFACE).read_bytes()
    inputs = [PROBE, *PACKAGE.glob("Package.swift"), *PACKAGE.glob("Sources/**/*")]
    hashes = {p: digest(p) for p in inputs if p.is_file()}
    copied = work / "apple/GhostermTerminal"
    shutil.copytree(PACKAGE, copied, ignore=shutil.ignore_patterns(".build", ".swiftpm"))
    for name in ("build", "core"):
        (work / name).symlink_to(ROOT / name, target_is_directory=True)
    probe = work / "probe"
    (probe / "Sources/Probe").mkdir(parents=True)
    shutil.copyfile(PROBE, probe / "Sources/Probe/main.swift")
    (probe / "Package.swift").write_text('''// swift-tools-version:5.9
import PackageDescription
let package = Package(name: "KittyMacProbe", platforms: [.macOS(.v13)],
    dependencies: [.package(path: "../apple/GhostermTerminal")],
    targets: [.executableTarget(name: "Probe", dependencies: [
        .product(name: "GhostermTerminal", package: "GhostermTerminal")])])
''')
    env = dict(os.environ, TMPDIR=str(ROOT / "tmp"))
    env.setdefault("DEVELOPER_DIR", "/Applications/Xcode.app/Contents/Developer")

    def verify(label, red=False):
        log = work / f"{label}-build.log"
        if run(["swift", "build", "--package-path", str(probe)], env, log, 300) != 0:
            raise RuntimeError(f"原生构建失败：{log}")
        output = work / label
        output.mkdir()
        log = work / f"{label}.log"
        code = run([str(probe / ".build/debug/Probe")],
                   dict(env, PROBE_OUT=str(output)), log, 60)
        lines = log.read_text().splitlines()
        expected = [f"PASS {CONTROL}", f"RESULT failures={8 if red else 0}"]
        expected += [f"{'FAIL' if red else 'PASS'} {name}" for name in TARGETS]
        if code != (1 if red else 0) or any(line not in lines for line in expected):
            raise RuntimeError(f"目标{'红' if red else '绿'}/独立文本对照判据不成立：{log}")
        print(f"PASS {label}：原生编译、8 项图形检查、独立文本对照", flush=True)

    try:
        verify("baseline")
        anchor = "    public func feed(_ data: Data) {\n"
        source = original.decode()
        if source.count(anchor) != 1:
            raise RuntimeError("变异锚点不唯一")
        # Only the isolated production surface is changed. Ordinary text still passes.
        (copied / SURFACE).write_text(source.replace(anchor, anchor +
            "        if data.starts(with: [0x1b, 0x5f, 0x47]) { return }\n"))
        verify("drop-graphics", red=True)
        (copied / SURFACE).write_bytes(original)
        verify("restored")
        if digest(copied / SURFACE) != hashes[PACKAGE / SURFACE]:
            raise RuntimeError("隔离副本 SHA256 未恢复")
        metadata = {"engine": receipt, "macOS": platform.mac_ver()[0],
                    "inputs": {str(p.relative_to(ROOT)): sha for p, sha in hashes.items()}}
        (work / "evidence.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print("PASS Kitty 图形：基线绿、编译后变异红、同变异文本绿、恢复绿、SHA256", flush=True)
    finally:
        (copied / SURFACE).write_bytes(original)
        if any(digest(path) != expected for path, expected in hashes.items()):
            raise RuntimeError("生产输入在验证期间发生变化，结果不可用于当前源码")


if __name__ == "__main__":
    def interrupted(number, frame):
        raise RuntimeError(f"Kitty 图形验证中断：{number}")

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()

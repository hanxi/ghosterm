#!/usr/bin/env python3
"""标签布局偏好的变异证据；超时统一经 gt_timeout，结束时逐字恢复源码。"""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "apple/GhostermApp"
PREFIX = "Sources/GhostermAppModel/"
CASES = [
    ("位置保存", "SettingsStore.swift",
     "defaults.set(newValue.rawValue, forKey: Key.tabPlacement.rawValue)",
     "defaults.set(TabPlacement.sidebar.rawValue, forKey: Key.tabPlacement.rawValue)",
     "testPlacementPersistsAndInvalidValueFallsBackToSidebar",
     "testCompactSidebarSurvivesTopPlacementAndUsesIndependentStore", "标签位置必须保存"),
    ("精简保存", "SettingsStore.swift",
     "defaults.set(newValue, forKey: Key.compactSidebar.rawValue)",
     "defaults.set(false, forKey: Key.compactSidebar.rawValue)",
     "testCompactSidebarSurvivesTopPlacementAndUsesIndependentStore",
     "testPlacementPersistsAndInvalidValueFallsBackToSidebar", "切到顶部不能丢掉精简偏好"),
    ("即时发布", "AppModel.swift",
     "@Published public var preferences: AppPreferences", "public var preferences: AppPreferences",
     "testModelPublishesLayoutChangesWithoutChangingTabs",
     "testPlacementPersistsAndInvalidValueFallsBackToSidebar", "布局设置必须立即通知已有视图"),
]


def main():
    def interrupted(signum, _):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    originals = {PKG / (PREFIX + case[1]): (PKG / (PREFIX + case[1])).read_bytes() for case in CASES}
    for path, data in originals.items():
        if b"GT_MUTATION_MARK" in data:
            raise SystemExit(f"残留变异标记：{path}")
    for _, name, old, *_ in CASES:
        if originals[PKG / (PREFIX + name)].decode().count(old) != 1:
            raise SystemExit(f"变异锚点必须唯一：{name}: {old}")
    hashes = {path: hashlib.sha256(data).hexdigest() for path, data in originals.items()}
    subprocess.run(["bash", "-c", 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tab-presentation', "bash", str(ROOT)], check=True)
    work = Path(tempfile.mkdtemp(prefix="tab-presentation-mutation-", dir=ROOT / "tmp"))
    env = dict(os.environ, TMPDIR=str(ROOT / "tmp") + "/", GHOSTERM_AUTOMATION_OFFSCREEN="1")

    def run(label, selector, green=True, diagnostic=None):
        log = work / (label + ".log")
        with log.open("w") as output:
            result = subprocess.run(["bash", "-c",
                'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tab-presentation; gt_timeout 300 swift test --package-path "$2" --filter "$3"',
                "bash", str(ROOT), str(PKG), "TabPresentationTests/" + selector],
                env=env, stdout=output, stderr=subprocess.STDOUT)
        text = log.read_text()
        valid = "Executed " in text and "Executed 0 tests" not in text
        if not valid or (result.returncode == 0) != green or (diagnostic and diagnostic not in text):
            print(text, flush=True)
            raise RuntimeError(f"证据不成立：{label}，完整日志 {log}")
        print(f"{label}: {'绿' if green else '目标红'}", flush=True)

    def restore():
        for path, data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[path]:
                raise RuntimeError(f"SHA256 回滚失败：{path}")

    try:
        for name, file, old, new, target, control, diagnostic in CASES:
            run(name + "-基线", target)
            run(name + "-对照基线", control)
            path = PKG / (PREFIX + file)
            path.write_text(originals[path].decode().replace(old, new + " /* GT_MUTATION_MARK */", 1))
            run(name + "-变异", target, green=False, diagnostic=diagnostic)
            run(name + "-同变异对照", control)
            restore()
            run(name + "-复原", target)
    finally:
        restore()
    print(f"tab-presentation PASS: {len(CASES)} 组目标红、同变异对照绿、回滚绿、SHA256 一致；日志 {work}")


if __name__ == "__main__":
    main()

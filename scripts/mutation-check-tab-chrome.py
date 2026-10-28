#!/usr/bin/env python3
"""标签界面重构的变异证据；超时统一经 gt_timeout，结束时逐字恢复源码。"""
import argparse
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PKG = ROOT / "apple/GhostermApp"

PREFERENCES = [
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

PREFERENCES = [(name, "Sources/GhostermAppModel/" + file, old, new,
                "TabPresentationTests/" + target, "TabPresentationTests/" + control, diagnostic)
               for name, file, old, new, target, control, diagnostic in PREFERENCES]
CONNECTIONS = [
    ("失效连接", "Sources/GhostermAppModel/ConnectionRequest.swift",
     "guard model.hosts.contains(where:", "guard true || model.hosts.contains(where:",
     "ConnectionChooserTests/testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected",
     "ConnectionChooserTests/testSearchMatchesAllTermsWithoutChangingHostAliases", "失效或通配别名不得建立连接"),
    ("文件新标签", "Sources/GhostermAppModel/ConnectionRequest.swift",
     "model.openLocalFiles(newTab: true)", "model.openLocalFiles(newTab: false)",
     "ConnectionChooserTests/testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected",
     "ConnectionChooserTests/testSearchMatchesAllTermsWithoutChangingHostAliases", "重复打开文件仍须新建标签"),
    ("读取错误", "Sources/GhostermAppModel/AppModel.swift",
     r'loadError = "读取 \(configPath) 失败：\(error)"', 'loadError = nil',
     "ConnectionChooserTests/testReadFailureClearsStaleHostsAndIsNotAnEmptyConfig",
     "ConnectionChooserTests/testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected", "读取失败不能报告成空配置"),
    ("旧通配块", "Sources/GhostermAppModel/AppModel.swift",
     "            wildcards = []", "            _ = wildcards",
     "ConnectionChooserTests/testReadFailureClearsStaleHostsAndIsNotAnEmptyConfig",
     "ConnectionChooserTests/testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected", "读取失败必须清掉旧的通配块"),
    ("错误提示", "Sources/GhostermApp/ConnectionChooser.swift",
     "if let error = model.loadError {\n                ConnectionLoadError", "if let error = model.loadError, false {\n                ConnectionLoadError",
     "ConnectionChooserTests/testReadErrorIsVisibleInBothChooserAndHostManager",
     "ConnectionChooserTests/testSearchMatchesAllTermsWithoutChangingHostAliases", "连接选择器必须画出读取错误"),
    ("主机管理菜单", "Sources/GhostermApp/AppMenu.swift",
     "        menu.addItem(hosts)", "        _ = hosts",
     "ConnectionChooserTests/testConnectionAndHostManagementMenuActionsPresentSheets",
     "ConnectionChooserTests/testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected", "主机管理入口必须保留"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", choices=["preferences", "connections", "all"], default="all")
    group = parser.parse_args().group
    cases = PREFERENCES if group == "preferences" else CONNECTIONS if group == "connections" else PREFERENCES + CONNECTIONS
    def interrupted(signum, _):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    originals = {PKG / case[1]: (PKG / case[1]).read_bytes() for case in cases}
    for path, data in originals.items():
        if b"GT_MUTATION_MARK" in data:
            raise SystemExit(f"残留变异标记：{path}")
    for _, name, old, *_ in cases:
        if originals[PKG / name].decode().count(old) != 1:
            raise SystemExit(f"变异锚点必须唯一：{name}: {old}")
    hashes = {path: hashlib.sha256(data).hexdigest() for path, data in originals.items()}
    subprocess.run(["bash", "-c", 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tab-chrome', "bash", str(ROOT)], check=True)
    work = Path(tempfile.mkdtemp(prefix="tab-chrome-mutation-", dir=ROOT / "tmp"))
    env = dict(os.environ, TMPDIR=str(ROOT / "tmp") + "/", GHOSTERM_AUTOMATION_OFFSCREEN="1")

    def run(label, selector, green=True, diagnostic=None):
        log = work / (label + ".log")
        with log.open("w") as output:
            result = subprocess.run(["bash", "-c",
                'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tab-chrome; gt_timeout 300 swift test --package-path "$2" --filter "$3"',
                "bash", str(ROOT), str(PKG), selector],
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
        for name, file, old, new, target, control, diagnostic in cases:
            run(name + "-基线", target)
            run(name + "-对照基线", control)
            path = PKG / file
            path.write_text(originals[path].decode().replace(old, new + " /* GT_MUTATION_MARK */", 1))
            run(name + "-变异", target, green=False, diagnostic=diagnostic)
            run(name + "-同变异对照", control)
            restore()
            run(name + "-复原", target)
    finally:
        restore()
    print(f"tab-chrome PASS: {len(cases)} 组目标红、同变异对照绿、回滚绿、SHA256 一致；日志 {work}")


if __name__ == "__main__":
    main()

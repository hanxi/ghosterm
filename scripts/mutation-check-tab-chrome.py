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

# 原 native-tabs / native-focus / native-context / native-sheet / native-titlebar
# 的业务证据迁到真实单窗口；不再依赖 AppKit 私有标签项或每标签 NSWindow。
GEOMETRY = "TabStripVisibilityTests/testNavigationGeometryForEmptySingleAndMultipleTabs"
ORDER = "TabWindowTests/testBothNavigationsSelectAndReorderByIdentity"
FOCUS = "TabWindowTests/testNavigationChangesKeepLiveHostsProcessesAndKeyboardFocus"
CLOSE = "TabWindowTests/testCloseButtonConfirmsTheTargetAndKeepsAnEmptyWindow"
NAVIGATION = [
    ("顶部空带", "Sources/GhostermApp/RootView.swift",
     "                panes\n", "                Color.clear.frame(height: showsSidebar ? WindowChrome.barHeight : 0)\n                panes\n",
     GEOMETRY, ORDER, "侧栏模式不能留下顶部空带"),
    ("重复导航", "Sources/GhostermApp/RootView.swift",
     "if model.preferences.tabPlacement == .top {", "if true {",
     GEOMETRY, "TabPresentationTests/testPlacementPersistsAndInvalidValueFallsBackToSidebar", "每种布局、空态和多标签都只能有一个加号"),
    ("红绿灯避让", "Sources/GhostermApp/NavigationControls.swift",
     "width: WindowChrome.trafficLightsWidth, height: 1", "width: 0, height: 1",
     GEOMETRY, ORDER, "加号不能遮住系统窗口按钮"),
    ("加号位置", "Sources/GhostermApp/Sidebar.swift",
     "NavigationControls(compact: compact)", "NavigationControls(compact: compact).offset(x: compact ? 0 : 80)",
     "TabStripVisibilityTests/testPlusStaysAtTheSamePositionInStandardLayoutsAndOpensChooser", ORDER, "两种标准布局的加号位置一致"),
    ("配置警告", "Sources/GhostermApp/RootView.swift",
     "if let error = model.terminalConfigError {", "if let error = model.terminalConfigError, false {",
     "TabStripVisibilityTests/testConfigurationWarningIsVisibleWithoutMovingThePlus", ORDER, "终端配置错误必须显示可见警告"),
    ("标签选择", "Sources/GhostermApp/TabInteraction.swift",
     ".onTapGesture { tabs.activate(tab) }", ".onTapGesture { _ = tab }",
     ORDER, GEOMETRY, "点击实际标签必须按 UUID 选中"),
    ("标签重排", "Sources/GhostermApp/TabInteraction.swift",
     "tabs.move(tab, to: index)", "_ = (tab, index)",
     ORDER, GEOMETRY, "重排必须更新同一标签清单"),
    ("关闭确认", "Sources/GhostermApp/RootView.swift",
     "            tabs.requestClose(tab)", "            tabs.close(tab)",
     CLOSE, ORDER, "关闭必须询问被点击的标签"),
]
LIFECYCLE = [
    ("保留终端宿主", "Sources/GhostermApp/RootView.swift",
     "                panes\n", "                panes.id(model.preferences.tabPlacement)\n",
     FOCUS, ORDER, "布局切换不得重建终端宿主"),
    ("标签焦点", "Sources/GhostermApp/RootView.swift",
     "isActive: tab.id == tabs.activeTabID)", "isActive: false)",
     FOCUS, ORDER, "切到第一页时键盘应落在该页终端"),
    ("窗口关闭", "Sources/GhostermApp/TabWindowController.swift",
     "        requestClose()", "        _ = sender",
     "TabWindowTests/testWindowCloseUsesTheApplicationExitRoute", ORDER, "窗口关闭必须走统一的退出确认入口"),
    ("退出裁决", "Sources/GhostermApp/TabWindowController.swift",
     "        return false", "        return true",
     "TabWindowTests/testWindowCloseUsesTheApplicationExitRoute", ORDER, "退出裁决前不能先销毁工作区"),
    ("唯一弹窗", "Sources/GhostermApp/RootView.swift",
     "panel == nil ? model.tmuxChooser : nil", "panel == nil && showsSidebar ? model.tmuxChooser : nil",
     "TabWindowTests/testTmuxSheetStaysOnTheSameWindowAcrossNavigationChanges", ORDER, "切换布局和标签不重开或迁移 sheet"),
    ("右键落点", "Sources/GhostermApp/TabInteraction.swift",
     "TabContextMenu(model: model, tabs: tabs, tab: tab)", "TabContextMenu(model: model, tabs: tabs, tab: tabs.activeTab ?? tab)",
     "TabNavigationContextTests/testBothNavigationsUseTheSameMenuAndTargetTheClickedTab", ORDER, "文件面板必须作用于右键的 UUID"),
]

MENU = [
    ("设置菜单", "Sources/GhostermApp/AppMenu.swift", "menu.addItem(settings)", "_ = settings",
     "AppMenuCoverageTests/testMenuBarKeepsEveryActionTheWindowNoLongerOffers", ORDER, "主菜单里没有「设置…」"),
    ("传输菜单", "Sources/GhostermApp/AppMenu.swift", "menu.addItem(transfers)", "_ = transfers",
     "AppMenuCoverageTests/testMenuBarKeepsEveryActionTheWindowNoLongerOffers",
     "SettingsUITests/testShortcutCatalogMatchesTheRealMenu", "主菜单里没有「传输」"),
    ("菜单接线", "Sources/GhostermApp/AppMenu.swift", "down.target = commands", "_ = commands",
     "AppMenuCoverageTests/testCommandItemsHaveAnExplicitTarget",
     "SettingsUITests/testShortcutCatalogMatchesTheRealMenu", "「向下分屏」没有 target"),
    ("关于打开", "Sources/GhostermApp/AppMenu.swift",
     "@objc func showAbout(_ sender: Any?) { AboutWindow.open() }", "@objc func showAbout(_ sender: Any?) {}",
     "AboutWindowTests/testAboutMenuItemOpensOurOwnWindow",
     "AppMenuCoverageTests/testMenuBarKeepsEveryActionTheWindowNoLongerOffers", "点了「关于 Ghosterm」却没有我们的关于窗口"),
    ("关于菜单", "Sources/GhostermApp/AppMenu.swift", "menu.addItem(about)", "_ = about",
     "AppMenuCoverageTests/testMenuBarKeepsEveryActionTheWindowNoLongerOffers", ORDER, "主菜单里没有「关于 Ghosterm」"),
]
SIDEBAR = [
    ("标签高亮", "Sources/GhostermApp/Sidebar.swift",
     "                .fill(isActive ? Color.primary.opacity(0.12) : Color.clear)", "                .fill(Color.clear)",
     "SidebarChromeTests/testActiveTabRowIsHighlighted", ORDER, "当前标签页那一行该有高亮块"),
    ("清除系统标题", "Sources/GhostermAppModel/WindowChrome.swift",
     'window.title = ""', 'window.title = "Ghosterm"',
     "WindowChromeTests/testApplyClearsWindowTitle", ORDER, "XCTAssertEqual failed"),
    ("横向滚轮", "Sources/GhostermApp/TabStrip.swift",
     ".background(WheelScrollsHorizontally())", ".background(Color.clear)",
     "TabStripScrollTests/testManyTabsOverflowAndWheelScrolls", ORDER, "WheelScrollsHorizontally 没挂到滚动区上"),
    ("滚轮坐标", "Sources/GhostermApp/HorizontalScrollAssist.swift",
     "return NSPoint(x: p.x, y: primaryHeight - p.y)", "return NSPoint(x: p.x, y: p.y)",
     "TabStripScrollTests/testQuartzScreenPointUsesAppKitVerticalAxis", ORDER, "XCTAssertEqual failed"),
]
SEARCH = [
    ("搜索选择目标", "Sources/GhostermApp/RootView.swift",
     "                    tabs.activate(tab)", "                    tabs.activate(tabs.activeTab ?? tab)",
     "TabSearchTests/testMenuShortcutSearchesSubtitlesAndSelectsDuplicateTitleByIdentity", ORDER,
     "搜索副标题后必须按 UUID 激活同名标签"),
    ("保留清屏", "Sources/GhostermApp/AppMenu.swift",
     '#selector(AppCommands.searchTabs(_:)), keyEquivalent: "A")\n        search.keyEquivalentModifierMask = [.command, .shift]',
     '#selector(AppCommands.searchTabs(_:)), keyEquivalent: "k")\n        search.keyEquivalentModifierMask = [.command]',
     "TabSearchTests/testCommandKStillClearsTheLiveTerminalWithoutOpeningSearch", ORDER,
     "⌘K 必须继续交给终端引擎"),
    ("搜索快捷键", "Sources/GhostermApp/AppMenu.swift",
     '#selector(AppCommands.searchTabs(_:)), keyEquivalent: "A")',
     '#selector(AppCommands.searchTabs(_:)), keyEquivalent: "a")',
     "TabSearchTests/testMenuShortcutSearchesSubtitlesAndSelectsDuplicateTitleByIdentity", ORDER,
     "搜索快捷键必须走真实主菜单"),
    ("搜索副标题", "Sources/GhostermApp/TabSearchView.swift",
     '$0.title + " " + $0.subtitle', '$0.title',
     "TabSearchTests/testMenuShortcutSearchesSubtitlesAndSelectsDuplicateTitleByIdentity", ORDER,
     "回车应确认搜索结果"),
    ("搜索实时标题", "Sources/GhostermApp/TabSearchView.swift",
     "titleRevision &+= 1", "_ = titleRevision",
     "TabSearchTests/testMenuShortcutSearchesSubtitlesAndSelectsDuplicateTitleByIdentity", ORDER,
     "回车应确认搜索结果"),
    ("顶部标题", "Sources/GhostermApp/TabStrip.swift", 'Text(tab.title)', 'Text("static")',
     "TabSearchTests/testTitlesRefreshInBothNavigationViewsWithoutReplacingTheirRows", ORDER,
     "真实标签标题必须立即重绘且 resize 不清空标题"),
    ("侧边标题", "Sources/GhostermApp/Sidebar.swift", 'Text(tab.title)', 'Text("static")',
     "TabSearchTests/testTitlesRefreshInBothNavigationViewsWithoutReplacingTheirRows", ORDER,
     "真实标签标题必须立即重绘且 resize 不清空标题"),
]

GROUPS = {"preferences": PREFERENCES, "connections": CONNECTIONS,
          "navigation": NAVIGATION, "lifecycle": LIFECYCLE, "menu": MENU, "sidebar": SIDEBAR,
          "search": SEARCH}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", choices=[*GROUPS, "all"], default="all")
    parser.add_argument("--case", help="仅运行名称匹配的一组，用于复验")
    args = parser.parse_args()
    cases = GROUPS[args.group] if args.group != "all" else sum(GROUPS.values(), [])
    if args.case:
        cases = [case for case in cases if case[0] == args.case]
        if not cases:
            parser.error("没有匹配的变异")
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

    mutated = {}

    def restore():
        for path, data in originals.items():
            current = path.read_bytes()
            if current != data:
                if current != mutated.get(path):
                    raise RuntimeError(f"源码出现非本轮修改，保留现场而非覆盖：{path}")
                path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashes[path]:
                raise RuntimeError(f"SHA256 回滚失败：{path}")
        mutated.clear()

    try:
        for name, file, old, new, target, control, diagnostic in cases:
            run(name + "-基线", target)
            run(name + "-对照基线", control)
            path = PKG / file
            mutated[path] = originals[path].decode().replace(old, new + " /* GT_MUTATION_MARK */", 1).encode()
            path.write_bytes(mutated[path])
            run(name + "-变异", target, green=False, diagnostic=diagnostic)
            run(name + "-同变异对照", control)
            restore()
            run(name + "-复原", target)
    finally:
        restore()
    print(f"tab-chrome PASS: {len(cases)} 组目标红、同变异对照绿、回滚绿、SHA256 一致；日志 {work}")


if __name__ == "__main__":
    main()

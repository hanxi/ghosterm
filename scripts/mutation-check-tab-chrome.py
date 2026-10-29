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
     "testPlacementUsesIndependentStore", "标签位置必须保存"),
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
    ("红绿灯避让", "Sources/GhostermApp/TabStrip.swift",
     "width: WindowChrome.trafficLightsWidth, height: 1", "width: 0, height: 1",
     GEOMETRY, ORDER, "顶部标签不能遮住系统窗口按钮"),
    ("顶部加号位置", "Sources/GhostermApp/TabStrip.swift",
     "NewConnectionButton(tint: ChromePalette(model: model).foreground).frame(width: DS.controlSize, height: DS.controlSize)",
     "NewConnectionButton(tint: ChromePalette(model: model).foreground).frame(width: DS.controlSize, height: DS.controlSize).offset(x: -80)",
     "TabStripVisibilityTests/testPlusFollowsTheLastSidebarTabAndStaysAtTheTopRight", ORDER, "顶部加号必须固定在最右端"),
    ("侧栏加号位置", "Sources/GhostermApp/Sidebar.swift",
     "NewConnectionButton(showsTitle: true, tint: ChromePalette(model: model).foreground)", "NewConnectionButton(showsTitle: true, tint: ChromePalette(model: model).foreground).offset(y: -60)",
     "TabStripVisibilityTests/testPlusFollowsTheLastSidebarTabAndStaysAtTheTopRight", ORDER, "侧栏加号必须位于最后一个标签下方"),
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
     "                .fill(isActive ? selection : Color.clear)", "                .fill(Color.clear)",
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

PANE_CONTROL = "PaneCommandTests/testStaleForeignAndHiddenTargetsNeverFallBackToTheActivePane"
PANE_HOVER = "PaneActionsTests/testHoverPreviewIsPassiveAndControlsReserveTheirWidth"
PANE_CANCEL = "PaneActionsTests/testPreviewClearsOnNavigationZoomDragEscapeAndTargetRemoval"
PANE_FOCUS = "PaneActionsTests/testPreviewTracksResizeAndStopsWhenWindowOrTabLosesFocus"
PANE_KEYS = "PaneActionsTests/testShiftShortcutsReachTheSplitAndFileCommands"
PANE_MENU = "PaneActionsTests/testTitleAndHiddenHandleMenusKeepThePaneTargetAndShortcuts"
PANES = [
    ("窗格分屏目标", "Sources/GhostermAppModel/AppModel.swift",
     "tab.activeWorkspacePaneID = paneID", "_ = paneID",
     "PaneCommandTests/testSplitTargetsTheRequestedTerminalAndInheritsItsDirectory", PANE_CONTROL,
     "指定窗格 B 的分屏只能替换 B 的叶子"),
    ("隐藏窗格拒绝", "Sources/GhostermAppModel/AppModel.swift",
     "tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true,",
     "true || tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true,",
     PANE_CONTROL, "PaneCommandTests/testFileSplitZoomAndCloseShareTheSameIdentityCommands",
     "失效目标不能改动所属标签的其他窗格"),
    ("文件关闭目标", "Sources/GhostermAppModel/AppModel.swift",
     "tab.removeFilePane(file.id)", "tab.removeFilePane(tab.activeWorkspacePaneID ?? file.id)",
     "PaneCommandTests/testFileSplitZoomAndCloseShareTheSameIdentityCommands", PANE_CONTROL,
     "关闭非当前文件窗格不能删掉当前窗格"),
    ("窗格悬停按钮", "Sources/GhostermApp/PaneActions.swift",
     "self.revealed = revealed;", "self.revealed = false;",
     PANE_HOVER, PANE_CONTROL, "悬停非当前窗格必须显示操作"),
    ("窗格键盘按钮", "Sources/GhostermApp/PaneActions.swift",
     "revealed || buttons.contains { $0.keyboardFocused }", "revealed",
     "PaneActionsTests/testKeyboardFocusRevealsActionsWithoutChangingTheActivePane", PANE_CONTROL,
     "键盘聚焦时必须显现操作"),
    ("预览不激活", "Sources/GhostermApp/WorkspaceView.swift",
     "splitPreview.show(paneID: paneID, axis: axis, content: content)",
     "tab?.activeWorkspacePaneID = paneID; splitPreview.show(paneID: paneID, axis: axis, content: content)",
     PANE_HOVER, PANE_CONTROL, "悬停预览不得激活目标窗格"),
    ("预览实际绘制", "Sources/GhostermApp/WorkspaceView.swift",
     "addSubview(splitPreview, positioned: .above, relativeTo: nil)", "_ = splitPreview",
     "PaneActionsTests/testPreviewIsActuallyPaintedOverTheWorkspace", PANE_CONTROL,
     "预览层必须真正绘制到工作区"),
    ("预览不挡输入", "Sources/GhostermApp/SplitPreview.swift",
     "override func hitTest(_ point: NSPoint) -> NSView? { nil }",
     "override func hitTest(_ point: NSPoint) -> NSView? { self }",
     PANE_HOVER, PANE_CONTROL, "覆盖层不得成为终端内容的命中目标"),
    ("预览方向", "Sources/GhostermApp/SplitPreview.swift",
     "axis == .horizontal", "axis == .vertical",
     PANE_HOVER, PANE_CONTROL, "向右预览必须只占目标窗格右半边"),
    ("离开清预览", "Sources/GhostermApp/PaneActions.swift",
     "override func mouseExited(with event: NSEvent) { hovered = false;",
     "override func mouseExited(with event: NSEvent) { hovered = true;",
     PANE_HOVER, PANE_CONTROL, "离开按钮必须清除预览"),
    ("窗口失焦保护", "Sources/GhostermApp/WorkspaceView.swift",
     "window?.isKeyWindow == true, tab?.isLocked == false, !isDraggingPane,",
     "tab?.isLocked == false, !isDraggingPane,",
     PANE_FOCUS, PANE_CONTROL, "后台窗口不能创建预览"),
    ("拖动预览互斥", "Sources/GhostermApp/WorkspaceView.swift",
     "tab?.isLocked == false, !isDraggingPane,", "tab?.isLocked == false,",
     PANE_CANCEL, PANE_CONTROL, "拖动期间不能重新出现分屏预览"),
    ("预览跟随尺寸", "Sources/GhostermApp/WorkspaceView.swift",
     "splitPreview.update(content: content)", "_ = content",
     PANE_FOCUS, PANE_CONTROL, "窗口变宽后预览必须重新计算"),
    ("分屏Shift按键", "Sources/GhostermApp/AppMenu.swift",
     'keyEquivalent: "D")', 'keyEquivalent: "d")',
     PANE_KEYS, PANE_CONTROL, "⇧⌘D 必须匹配真实带 Shift 的按键事件"),
    ("文件Shift按键", "Sources/GhostermApp/AppMenu.swift",
     'keyEquivalent: "E")', 'keyEquivalent: "e")',
     PANE_KEYS, PANE_CONTROL, "⇧⌘E 必须匹配真实带 Shift 的按键事件"),
    ("窗格菜单目标", "Sources/GhostermApp/PaneActions.swift",
     "actions.preview(nil); item.perform()",
     'actions.preview(nil); if item.id == "split-down" { actions.model.splitActiveTab(axis: .vertical) } else { item.perform() }',
     PANE_MENU, PANE_CONTROL, "非当前窗格右键不得分屏当前窗格"),
    ("隐藏把手菜单", "Sources/GhostermApp/RootView.swift",
     "WorkspaceHiddenHandle(tab: tab, paneID: pane.id)\n                        .contextMenu { PaneContextMenu(actions: actions) }",
     "WorkspaceHiddenHandle(tab: tab, paneID: pane.id)",
     PANE_MENU, PANE_CONTROL, "实际命中范围内应提供右键菜单"),
    ("窗格菜单快捷键", "Sources/GhostermApp/PaneActions.swift",
     ".keyboardShortcut(item.shortcut)", ".keyboardShortcut(nil)",
     PANE_MENU, PANE_CONTROL, "窗格菜单必须显示真实分屏快捷键"),
    ("文件动作旧标签", "Sources/GhostermApp/PaneActions.swift",
     "guard model.tabs.tabs.contains(where: { $0.id == tab.id }),", "guard true,",
     "PaneActionsTests/testRemoteFileActionRejectsHiddenOrRemovedTargets", PANE_CONTROL,
     "已移除标签的动作不得回落到当前标签"),
    ("文件动作隐藏目标", "Sources/GhostermApp/PaneActions.swift",
     "tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true else { return }",
     "true || tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true else { return }",
     "PaneActionsTests/testRemoteFileActionRejectsHiddenOrRemovedTargets", PANE_CONTROL,
     "隐藏文件窗格的旧动作不得激活标签"),
    ("文件分屏键盘落点", "Sources/GhostermApp/WorkspaceView.swift",
     "if isActive, focusMoved, let button", "if false, focusMoved, let button",
     "PaneActionsTests/testFilePaneUsesTheSamePreviewAndNativeActions", PANE_HOVER,
     "文件分屏后键盘必须交给新文件列表"),
    ("文件按钮保留键盘", "Sources/GhostermApp/WorkspaceView.swift",
     "!(window.firstResponder is PaneActionButton),", "true,",
     "PaneActionsTests/testFilePaneUsesTheSamePreviewAndNativeActions", PANE_HOVER,
     "文件刷新不得抢走非当前窗格按钮的键盘"),
]

APPEARANCE = [
    ("分隔线留白", "Sources/GhostermApp/WorkspaceView.swift",
     "separatorColor = opts.gap == 0 ? ChromePalette(model: model).separator : .clear",
     "separatorColor = .black",
     "ChromePaletteTests/testGapOnlyChangesSpacingAndDividerInk",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "留白时分隔区域必须透明"),
    ("默认贴边", "Sources/GhostermAppModel/PaneAppearance.swift",
     "public static let defaultValue = 0", "public static let defaultValue = 10",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "默认窗格必须贴边，不能有外留白"),
    ("导航同色", "Sources/GhostermApp/Sidebar.swift",
     ".modifier(ChromeSurface(palette: ChromePalette(model: model)))",
     ".background(Color(nsColor: .windowBackgroundColor))",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "导航必须使用终端背景色"),
    ("标题去线", "Sources/GhostermApp/PaneChrome.swift",
     ".modifier(ChromeSurface(palette: palette))",
     ".modifier(ChromeSurface(palette: palette)).overlay(alignment: .bottom) { Divider() }",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "窗格标题不能有底部描边"),
    ("移除外框", "Sources/GhostermApp/PaneChrome.swift",
     ".clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))",
     ".clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous)).overlay(Rectangle().strokeBorder(Color.accentColor, lineWidth: 1))",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "窗格不能绘制整圈焦点描边"),
    ("条件主题颜色", "../GhostermTerminal/Sources/CGhostty/shim.c",
     "a->has_resolved_colors = true;", "a->has_resolved_colors = false;",
     "ChromePaletteTests/testConditionalThemeUsesResolvedColors",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload", "条件主题切换必须更新导航底色"),
    ("按钮前景色", "Sources/GhostermApp/NavigationControls.swift",
     "        button.contentTintColor = tint\n        button.attributedTitle",
     "        button.contentTintColor = .labelColor\n        button.attributedTitle",
     "ChromePaletteTests/testNavigationTitleAndTerminalShareThemeAfterReload",
     "PaneChromeTests/testFocusedPaneTitleBarHasShortMarker", "原生加号必须跟随终端前景色"),
]

GROUPS = {"preferences": PREFERENCES, "connections": CONNECTIONS,
          "navigation": NAVIGATION, "lifecycle": LIFECYCLE, "menu": MENU, "sidebar": SIDEBAR,
          "search": SEARCH, "panes": PANES, "appearance": APPEARANCE}


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

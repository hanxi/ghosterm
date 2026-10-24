// TabContextMenuTests — 标签右键菜单的两条命令接缝（用户建议）。
//
// 用户反馈：「目前只有快捷键打开 SFTP 侧边栏，可以考虑在 tab 栏的对应标签加
// 右键菜单，可以把分割窗口的按钮也收到标签的邮件菜单里。」
//
// 右键菜单的视图本身（`TabContextMenu`）不好逐项断言，但它调的两条**命令接缝**
// 可以：`AppModel.split(_:axis:)` 与 `AppModel.toggleFilesPanel(for:)`。
// 两条都必须作用在**被右键的那条标签**上，而不是「当前标签页」——右键的那条
// 未必是当前标签页，写错就会「右键 A 却动了 B」，不报错、只在屏幕上不对。
//
// 另钉一条「菜单上看得见快捷键」：`.keyboardShortcut` 必须真的变成 AppKit
// 那张 NSMenu 上的 keyEquivalent（不然菜单右侧什么都不画，用户只能靠记），
// 且键位要与 `ShortcutCatalog`（已与主菜单双向对拍）逐条一致——否则菜单会
// 标出一个按了没反应的键，比不标更糟。
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor
final class TabContextMenuTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "ghosterm-ctx-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                            encoding: .utf8)
        return isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")
    }

    /// 分屏作用在**被右键的那条**标签上（不是当前标签页）。
    func testSplitTargetsTheRightClickedTab() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let a = TerminalTab(kind: .localShell, pane: TerminalPane())
        let b = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(a)
        model.tabs.add(b)
        model.tabs.activate(a)              // 当前是 A

        model.split(b, axis: .horizontal)   // 右键 B → 分屏 B
        XCTAssertEqual(b.panes.count, 2, "分屏该落在被右键的标签 B 上")
        XCTAssertEqual(a.panes.count, 1, "当前标签页 A 不该被顺手分屏")
        XCTAssertEqual(model.tabs.activeTabID, b.id,
                       "对某条标签分屏应把它带到前台（分屏的落点是当前窗格）")
    }

    /// 文件面板开关作用在**被右键的那条**标签上，并把这条带到前台。
    func testToggleFilesPanelTargetsTheRightClickedTab() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let a = TerminalTab(kind: .localShell, pane: TerminalPane())
        let b = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(a)
        model.tabs.add(b)
        model.tabs.activate(a)

        model.toggleFilesPanel(for: b)      // 右键 B → 显示文件面板
        XCTAssertTrue(b.filesPanelVisible, "面板该开在被右键的标签 B 上")
        XCTAssertFalse(a.filesPanelVisible, "当前标签页 A 不该被顺手开面板")
        XCTAssertEqual(model.tabs.activeTabID, b.id, "开面板应把 B 带到前台")

        model.toggleFilesPanel(for: b)
        XCTAssertFalse(b.filesPanelVisible, "再切换应收起")
    }

    /// SFTP 标签页没有内嵌文件面板（它本身就是双栏文件管理器），开关是 no-op。
    func testToggleFilesPanelIsNoOpForSFTPTab() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let sftp = TerminalTab(kind: .sftp(RemoteTarget(pattern: "web", user: "u",
                                                        host: "h", port: 22)))
        model.tabs.add(sftp)
        model.toggleFilesPanel(for: sftp)
        XCTAssertTrue(sftp.filesPanelVisible, "纯文件工作区不能隐藏为空白")
        XCTAssertNil(sftp.filesPanel)
    }

    /// 菜单项上的快捷键必须与 `ShortcutCatalog` 一致（没有的就不许标）。
    func testMenuItemsShowTheRealShortcuts() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)

        let items = try menuItems {
            TabContextMenu(model: model, tabs: model.tabs, tab: tab)
        }
        let titles = items.map(\.title)
        // 未分屏的标签页：没有「缩放当前分屏」「关闭当前分屏」（只有一格时
        // 这两条无意义）。拷贝/导出是从顶栏 `⋯` 并过来的。
        XCTAssertEqual(titles, ["显示文件面板", "向右分屏", "向下分屏",
                                "打开本地文件窗格", "恢复默认布局",
                                "拷贝文本", "导出文本…",
                                "锁定标签页", "断开并保留会话", "关闭标签页"],
                       "菜单项（去掉分隔线）应是这几条")

        // 菜单项标题 → `ShortcutCatalog` 里同一个动作的标题（措辞不同，动作同一个）。
        let alias = ["显示文件面板": "文件面板", "隐藏文件面板": "文件面板"]
        var catalog: [String: String] = [:]
        for g in ShortcutCatalog.groups {
            for i in g.items { catalog[i.title] = i.keys }
        }
        for item in items {
            let keys = ShortcutCatalog.describe(
                keyEquivalent: item.keyEquivalent,
                modifiers: item.keyEquivalentModifierMask)
            let expected = catalog[alias[item.title] ?? item.title] ?? "无"
            XCTAssertEqual(keys, expected,
                           "「\(item.title)」该标 \(expected)，实际 \(keys)")
        }
    }

    /// 已分屏的标签页：多出「缩放当前分屏 ⇧⌘↩」与「关闭当前分屏」两条，
    /// 且键位仍与 `ShortcutCatalog` 一致。
    ///
    /// ⇧⌘↩ 这条特别容易漏在菜单上：`\r` 不像字母那样「写什么画什么」，
    /// 菜单右侧要画的是 ↩（见 ShortcutCatalog.glyphs）。
    func testSplitTabMenuPinsZoomShortcut() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        model.split(tab, axis: .horizontal)

        let items = try menuItems {
            TabContextMenu(model: model, tabs: model.tabs, tab: tab)
        }
        let titles = items.map(\.title)
        XCTAssertEqual(titles, ["显示文件面板", "向右分屏", "向下分屏",
                                "缩放当前分屏", "关闭当前分屏",
                                "打开本地文件窗格", "恢复默认布局",
                                "拷贝文本", "导出文本…",
                                "锁定标签页", "断开并保留会话", "关闭标签页"],
                       "分屏后应多出缩放 / 关闭当前分屏两条")

        let zoom = try XCTUnwrap(items.first { $0.title == "缩放当前分屏" })
        XCTAssertEqual(ShortcutCatalog.describe(
            keyEquivalent: zoom.keyEquivalent,
            modifiers: zoom.keyEquivalentModifierMask), "⇧⌘↩",
            "缩放该标 ⇧⌘↩（与 ShortcutCatalog 一致）")
    }

    /// 装进 NSHostingView，取 AppKit 侧**真的那张** NSMenu（分隔线已剔除）。
    /// 「菜单上有没有画出快捷键」只有这一层看得见。
    private func menuItems<V: View>(@ViewBuilder _ content: () -> V) throws -> [NSMenuItem] {
        // 底衬用实色：透明视图上 SwiftUI 不一定把菜单挂上来（实测 Color.clear 取不到）。
        let host = NSHostingView(rootView:
            Rectangle().fill(Color.gray).frame(width: 120, height: 80)
                .contextMenu(menuItems: content))
        host.frame = NSRect(x: 0, y: 0, width: 120, height: 80)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled],
                              backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        for _ in 0..<4 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }

        let click = NSEvent.mouseEvent(with: .rightMouseDown,
                                       location: NSPoint(x: 60, y: 40),
                                       modifierFlags: [], timestamp: 0,
                                       windowNumber: window.windowNumber,
                                       context: nil, eventNumber: 0,
                                       clickCount: 1, pressure: 1)
        let event = try XCTUnwrap(click)
        let menu = try XCTUnwrap(Self.findMenu(host, event), "取不到右键菜单")
        return menu.items.filter { !$0.isSeparatorItem }
    }

    private static func findMenu(_ view: NSView, _ event: NSEvent) -> NSMenu? {
        if let m = view.menu(for: event), !m.items.isEmpty { return m }
        for sub in view.subviews {
            if let m = findMenu(sub, event) { return m }
        }
        return nil
    }

    /// 对某条标签关当前分屏：两个窗格 → 关掉一个。
    func testClosePaneInTargetsTheRightClickedTab() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        model.split(tab, axis: .horizontal)
        XCTAssertEqual(tab.panes.count, 2)
        model.closePane(in: tab)
        XCTAssertEqual(tab.panes.count, 1, "两个窗格时关当前分屏应剩一个")
    }
}

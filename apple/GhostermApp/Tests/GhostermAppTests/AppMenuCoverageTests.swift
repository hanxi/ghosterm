// AppMenuCoverageTests — 菜单栏是窗口内按钮删掉之后**唯一**的退路。
//
// 背景：顶带原来挂着两个入口——侧栏底部的 ⚙︎「设置」按钮，和标签条右端的
// ⋯ 溢出菜单（里面是新建标签页 / 关闭窗口 / 设置… / 分屏那几项的副本）。
// 两个都删了，理由是 macOS 顶上本来就有全局菜单条，窗口里再挂一份是重复。
// 这个前提只有在菜单条真的齐全时才成立，所以把它钉在这儿：哪天有人顺手从
// `AppMenu` 里拿掉一项，窗口里又没有第二条路，那个功能就是真的消失了，
// 而且谁也不会发现——编译照过，界面照画。
//
// 与 `SettingsUITests.testShortcutCatalogMatchesTheRealMenu` 的分工：那条从
// 「键盘页列出的快捷键」出发对拍，只覆盖**有快捷键**的项；「传输」「关闭
// 当前分屏」没有快捷键，不在那张表里，正是这里要补的缺口。
//
// 变异验证见 `scripts/mutation-check-tab-chrome.py --group menu`。
import AppKit
import GhostermAppModel
import XCTest
@testable import GhostermApp

final class AppMenuCoverageTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    @MainActor
    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "ghosterm-appmenu-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                             encoding: .utf8)
        return isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts",
                        settings: SettingsStore(terminalConfigPath: dir + "/ghostty.conf"))
    }

    /// 摊平主菜单：标题 → 菜单项。
    @MainActor
    private func installedItems() throws -> [String: NSMenuItem] {
        let model = try makeModel()
        AppMenu.install(model: model)
        addTeardownBlock { Task { @MainActor in NSApp.mainMenu = nil } }

        var byTitle: [String: NSMenuItem] = [:]
        func walk(_ menu: NSMenu) {
            for item in menu.items {
                if !item.title.isEmpty { byTitle[item.title] = item }
                if let sub = item.submenu { walk(sub) }
            }
        }
        if let main = NSApp.mainMenu { walk(main) }
        return byTitle
    }

    /// 窗口里已经没有按钮的那些动作，菜单栏里必须都还在，且都点得动。
    @MainActor
    func testMenuBarKeepsEveryActionTheWindowNoLongerOffers() throws {
        let items = try installedItems()

        // 这些是「窗口里删掉的 ⚙︎ 与 ⋯ 曾经提供过的」+「从来只有菜单栏
        // 有的」。少一条，用户就彻底够不着那个功能了。
        let required = [
            "设置…", "传输", "关闭窗口", "关闭标签页", "新建标签页",
            "向右分屏", "向下分屏", "缩放当前分屏", "关闭当前分屏",
            "文件面板", "切换标签位置", "关于 Ghosterm",
        ]
        for title in required {
            guard let item = items[title] else {
                XCTFail("主菜单里没有「\(title)」——窗口里的 ⚙︎ 和 ⋯ 已经删了，"
                        + "菜单栏是最后一条路，这项功能现在无法触达")
                continue
            }
            XCTAssertNotNil(item.action,
                            "「\(title)」在菜单里，但没有 action——点了什么都不会发生")
            XCTAssertFalse(item.isHidden, "「\(title)」被藏起来了")
        }
    }

    /// 走 `AppCommands` 的项必须有 target。
    ///
    /// 没有 target 的菜单项会沿响应者链找人接，而 `AppCommands` 不在链上，
    /// 于是点了**什么都不发生**——「向下分屏」真的漏过这一行（见
    /// `AppMenu.viewMenuItem` 的注释）。上一条只验「action 不为 nil」，
    /// 挡不住这种漏法。
    @MainActor
    func testCommandItemsHaveAnExplicitTarget() throws {
        let items = try installedItems()

        let needsTarget = [
            "设置…", "传输", "关闭标签页", "新建标签页",
            "向右分屏", "向下分屏", "缩放当前分屏", "关闭当前分屏",
            "文件面板", "切换标签位置",
            // 「关于」也走 AppCommands：开的是我们自己的关于窗口，不是系统
            // 那张只认 Info.plist 的标准面板（列不出随包组件的许可）。
            "关于 Ghosterm",
        ]
        for title in needsTarget {
            guard let item = items[title] else { continue }  // 上一条已经报过
            XCTAssertNotNil(item.target,
                            "「\(title)」没有 target——AppCommands 不在响应者链上，"
                            + "点它不会有任何反应")
        }
        // 「关闭窗口」刻意**没有** target：它走系统的 performClose，
        // 由当前窗口接，这样才会触发关窗确认那一套。
        XCTAssertNil(items["关闭窗口"]?.target,
                     "「关闭窗口」不该绑 target，否则绕开窗口自己的关闭确认")
    }

    /// 「设置…」真的会把打开设置的意图广播出去。
    ///
    /// 窗口里的 ⚙︎ 删掉以后，这条通知是设置面板唯一的开门方式；只验菜单项
    /// 存在是不够的，`showSettings` 改成发别的名字一样会悄悄坏掉。
    @MainActor
    func testSettingsItemBroadcastsTheOpenIntent() throws {
        let items = try installedItems()
        let settings = try XCTUnwrap(items["设置…"], "菜单栏里没有「设置…」")

        let got = expectation(description: "ghostermShowSettings")
        let token = NotificationCenter.default.addObserver(
            forName: .ghostermShowSettings, object: nil, queue: .main) { _ in
                got.fulfill()
            }
        defer { NotificationCenter.default.removeObserver(token) }

        let target = try XCTUnwrap(settings.target as? NSObject)
        let action = try XCTUnwrap(settings.action)
        XCTAssertTrue(target.responds(to: action), "设置…的 target 不认识它的 action")
        target.perform(action, with: settings)

        wait(for: [got], timeout: 2)
    }
}

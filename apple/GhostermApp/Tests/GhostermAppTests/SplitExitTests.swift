// SplitExitTests — 分屏窗格**退出后的折叠**，以及**结构改动后的分隔尺寸**。
//
// 两条都由用户实测 / 推敲带出：
//   1. 分屏里某个 shell `exit` 后，原实现把窗格留在「已退出」、占着半屏死着
//      ——应**立即收掉**、布局塌陷（对齐 iTerm2 / Ghostty / tmux）。只剩一个
//      窗格时收掉它 = 关标签页。
//   2. 关掉一个窗格后，剩下的窗格停在上一次布局给的**不相等**宽度（实测三窗格
//      关中间 → 163.5 vs 735.5，而不是各半 450）——`NSSplitView` 不重算分隔。
//      修法：分屏容器按**结构 id**（`PaneLayout.viewID`）强制重建（见 RootView）。
//
// 结构决策走 `AppModel.handlePaneExit`（纯结构、不建真会话）；折叠真的从
// 「shell 退出」触发这条另有一条端到端（真本地 PTY 敲 `exit`）。
import AppKit
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class SplitExitTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "split-exit-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/ssh_config", atomically: true,
                               encoding: .utf8)
        return isolatedAppModel(configPath: dir + "/ssh_config",
                        knownHostsPath: dir + "/known_hosts")
    }

    // MARK: - 退出 = 收窗格 / 关标签页（结构决策）

    /// 分屏里一个窗格退出 → 收掉它、布局塌陷、当前窗格收敛到剩下那个。
    func testExitCollapsesSplit() throws {
        let model = try makeModel()
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        let other = TerminalPane()
        tab.split(axis: .horizontal, with: other)     // 当前窗格 = other
        model.tabs.add(tab)
        let exiting = tab.panes[0]

        model.handlePaneExit(exiting)

        XCTAssertEqual(tab.panes.count, 1, "退出的窗格应被收掉（不留死窗格）")
        XCTAssertTrue(tab.panes[0] === other, "剩下的是另一个窗格")
        XCTAssertEqual(tab.activePaneID, other.id, "当前窗格应收敛到剩下那个")
        XCTAssertEqual(model.tabs.tabs.count, 1, "还有窗格，标签页不该被关")
    }

    /// 最后一个窗格退出 → 关标签页（不留一个「已退出」的空标签页）。
    func testLastPaneExitClosesTab() throws {
        let model = try makeModel()
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)

        model.handlePaneExit(tab.panes[0])

        XCTAssertTrue(model.tabs.tabs.isEmpty, "最后一个窗格退出应关掉标签页")
        XCTAssertNil(model.tabs.activeTabID, "没有标签页了 → 空态")
    }

    /// 只关「退出的那个」标签页，别动邻居。
    func testLastPaneExitOnlyClosesThatTab() throws {
        let model = try makeModel()
        let a = TerminalTab(kind: .localShell, pane: TerminalPane())
        let b = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(a); model.tabs.add(b)
        XCTAssertEqual(model.tabs.activeTabID, b.id)

        model.handlePaneExit(a.panes[0])

        XCTAssertEqual(model.tabs.tabs.map(\.id), [b.id], "只该关退出的那个")
        XCTAssertEqual(model.tabs.activeTabID, b.id, "剩下的标签页是当前")
    }

    /// 后台标签页里的分屏退出也要收（按**对象**找，不假定是当前标签页）。
    func testExitInBackgroundTabCollapses() throws {
        let model = try makeModel()
        let back = TerminalTab(kind: .localShell, pane: TerminalPane())
        back.split(axis: .vertical, with: TerminalPane())
        let front = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(back); model.tabs.add(front)   // front 是当前
        XCTAssertEqual(model.tabs.activeTabID, front.id)

        model.handlePaneExit(back.panes[0])

        XCTAssertEqual(back.panes.count, 1, "后台分屏退出也要塌陷")
        XCTAssertEqual(model.tabs.activeTabID, front.id, "不该抢走当前标签页")
    }

    /// 已经不在任何标签页里的窗格（用户先关了）→ 什么都不做（不崩、不误关别人）。
    func testExitOfOrphanPaneIsNoop() throws {
        let model = try makeModel()
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        let orphan = TerminalPane()
        model.handlePaneExit(orphan)
        XCTAssertEqual(model.tabs.tabs.map(\.id), [tab.id], "不该误伤在册标签页")
    }

    // MARK: - 端到端：真本地 shell 敲 exit → 真的被收掉

    private func waitExit(_ pane: TerminalPane, timeout: TimeInterval = 12) {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if pane.status.isExited { return }
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
    }

    /// 分屏里一个窗格退出 → 收掉它、布局塌陷（原生持久会话也要走这条）。
    func testRealShellExitCollapsesPane() throws {
        let model = try makeModel(); defer { model.disconnectForTesting() }
        model.connect(to: .localShell)
        guard let tab = model.tabs.activeTab else { return XCTFail("应有标签页") }
        model.splitActiveTab(axis: .horizontal)
        let exiting = tab.panes[0]
        let ready = Date().addingTimeInterval(10)
        while exiting.status != .ready, Date() < ready { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
        XCTAssertEqual(exiting.status, .ready)
        exiting.surface?.sendCommandLine("exit")
        waitExit(exiting)
        XCTAssertTrue(exiting.status.isExited)
        // 退出 → 回调 → 收窗格（主线程 hop 一步）。
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && tab.panes.count > 1 {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertEqual(tab.panes.count, 1, "退出的窗格应被自动收掉（还剩 \(tab.panes.count)）")
        XCTAssertFalse(tab.panes.contains { $0 === exiting }, "那个窗格不该还在")
        XCTAssertEqual(model.tabs.tabs.count, 1, "还有窗格，标签页不该被关")
    }

    /// 最后一个窗格退出 → 关标签页（不留「已退出」空标签页）。
    func testRealShellExitClosesLastPaneTab() throws {
        let model = try makeModel(); defer { model.disconnectForTesting() }
        model.connect(to: .localShell)
        guard let tab = model.tabs.activeTab, let pane = tab.activePane else { return XCTFail("应有窗格") }
        let ready = Date().addingTimeInterval(10)
        while pane.status != .ready, Date() < ready { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
        XCTAssertEqual(pane.status, .ready)
        pane.surface?.sendCommandLine("exit")
        waitExit(pane)
        XCTAssertTrue(pane.status.isExited)
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && !model.tabs.tabs.isEmpty {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertTrue(model.tabs.tabs.isEmpty, "最后一个 shell 退出应关掉标签页")
        XCTAssertNil(model.tabs.activeTabID, "空态")
        XCTAssertFalse(pane.isRunning, "关掉的标签页不该留着会话")
    }
}

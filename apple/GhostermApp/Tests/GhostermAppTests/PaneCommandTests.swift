import AppKit
import GhostermAppModel
import XCTest
@testable import GhostermApp

@MainActor final class PaneCommandTests: XCTestCase {
    func testSplitTargetsTheRequestedTerminalAndInheritsItsDirectory() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let (tab, surface) = try f.shell("Target")
        let source = try XCTUnwrap(tab.activePane)
        XCTAssertTrue(f.until { source.currentDirectory == f.directory.path })
        f.settle()
        let sibling = TerminalPane()
        tab.split(axis: .horizontal, with: sibling)
        let original = try XCTUnwrap(tab.workspaceLayout)
        let (other, _) = try f.shell("Other")
        f.settle()
        f.model.split(tabID: tab.id, paneID: source.id, axis: .vertical)
        let created = try XCTUnwrap(tab.activePane)
        XCTAssertFalse(created === source || created === sibling)
        XCTAssertEqual(f.model.tabs.activeTabID, tab.id, "点击非当前标签的窗格动作时才激活所属标签")
        let area = CGRect(x: 0, y: 0, width: 900, height: 600)
        let before = original.geometry(in: area, minimum: { _ in CGSize(width: 20, height: 20) }).panes
        let after = try XCTUnwrap(tab.workspaceLayout).geometry(in: area, minimum: { _ in CGSize(width: 20, height: 20) }).panes
        let old = try XCTUnwrap(before[source.id]), kept = try XCTUnwrap(after[source.id]), added = try XCTUnwrap(after[created.id])
        XCTAssertEqual(after[sibling.id], before[sibling.id], "指定窗格 B 的分屏只能替换 B 的叶子")
        XCTAssertEqual(kept.minX, old.minX); XCTAssertEqual(added.minX, old.minX)
        XCTAssertEqual(kept.width, old.width); XCTAssertEqual(added.width, old.width)
        XCTAssertEqual(kept.minY, old.minY); XCTAssertEqual(added.minY, old.midY)
        XCTAssertEqual(kept.height + added.height, old.height)
        XCTAssertEqual(other.panes.count, 1, "不得分屏当前的另一个标签")
        XCTAssertTrue(source.surface === surface, "分屏保留原终端 surface")
        let output = try XCTUnwrap(created.surface)
        output.sendCommandLine("printf 'GT_PANE_CWD_%s\\n' \"$PWD\"")
        XCTAssertTrue(f.until { output.readText()?.contains("GT_PANE_CWD_" + f.directory.path) == true },
                      "新本地窗格必须继承被操作窗格的目录")
        XCTAssertEqual(tab.activeWorkspacePaneID, created.id, "完成分屏后聚焦新窗格")
    }

    func testStaleForeignAndHiddenTargetsNeverFallBackToTheActivePane() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let a = TerminalTab(kind: .localShell, pane: TerminalPane())
        let b = TerminalTab(kind: .localShell, pane: TerminalPane())
        f.model.tabs.add(a); f.model.tabs.add(b)
        let hidden = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend()))
        a.addFilePane(hidden); a.filesPanelVisible = false
        let layoutA = a.workspaceLayout, layoutB = b.workspaceLayout
        let activeA = a.activeWorkspacePaneID, activeB = b.activeWorkspacePaneID
        let bPane = try XCTUnwrap(b.activePane)
        for (tabID, paneID) in [(UUID(), bPane.id), (a.id, UUID()), (a.id, bPane.id), (a.id, hidden.id)] {
            f.model.split(tabID: tabID, paneID: paneID, axis: .horizontal)
            f.model.toggleZoom(tabID: tabID, paneID: paneID)
            f.model.closePane(tabID: tabID, paneID: paneID)
            XCTAssertEqual(a.workspaceLayout, layoutA, "失效目标不能改动所属标签的其他窗格")
            XCTAssertEqual(b.workspaceLayout, layoutB, "失效目标不能回落到当前窗格")
            XCTAssertEqual(f.model.tabs.activeTabID, b.id)
            XCTAssertEqual(a.activeWorkspacePaneID, activeA)
            XCTAssertEqual(b.activeWorkspacePaneID, activeB)
            XCTAssertNil(f.model.tabs.closeRequest)
        }
    }

    func testFileSplitZoomAndCloseShareTheSameIdentityCommands() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        f.model.openLocalFiles(newTab: true)
        let tab = try XCTUnwrap(f.model.tabs.activeTab)
        let source = try XCTUnwrap(tab.activeWorkspacePane?.files)
        let unrelated = TerminalTab(kind: .localFiles)
        f.model.tabs.add(unrelated)
        let unrelatedPanes = unrelated.workspacePanes.map(\.id)
        f.model.split(tabID: tab.id, paneID: source.id, axis: .vertical)
        let copy = try XCTUnwrap(tab.activeWorkspacePane?.files)
        XCTAssertNotEqual(copy.id, source.id, "文件分屏须创建独立浏览器")
        XCTAssertTrue(copy.panel?.backend === source.panel?.backend)
        XCTAssertEqual(copy.panel?.currentPath, source.panel?.currentPath)
        XCTAssertEqual(f.model.tabs.activeTabID, tab.id)
        f.model.toggleZoom(tabID: tab.id, paneID: source.id)
        XCTAssertEqual(tab.zoomedPaneID, source.id, "缩放必须使用所属窗格 UUID")
        f.model.closePane(tabID: tab.id, paneID: copy.id)
        XCTAssertEqual(tab.filePanes.map(\.id), [source.id], "关闭非当前文件窗格不能删掉当前窗格")
        XCTAssertNil(tab.zoomedPaneID)
        XCTAssertEqual(unrelated.workspacePanes.map(\.id), unrelatedPanes)
    }
}

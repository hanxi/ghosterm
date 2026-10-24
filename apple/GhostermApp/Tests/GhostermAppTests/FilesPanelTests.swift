// FilesPanelTests — 文件浏览器接缝的本地侧（无网可跑）。
//
// 覆盖：LocalFileBackend 的 list/mkdir/rename/chmod/remove(递归与非递归)/walk；
// 符号链接三态（kind=.symlink）；FilesPanelModel 的导航/排序/过滤/隐藏/选择/
// 新建/重命名/删除；glob 匹配口径。
import AppKit
import GhostermAppModel
import XCTest

final class FilesPanelTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func tmpDir() throws -> String {
        let d = NSTemporaryDirectory() + "filespanel-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: d) }
        return d
    }

    private func touch(_ path: String, _ bytes: Int = 0) throws {
        try Data(repeating: 0x41, count: bytes).write(to: URL(fileURLWithPath: path))
    }

    // MARK: - LocalFileBackend

    func testLocalListAndKinds() throws {
        let root = try tmpDir()
        try touch(root + "/file.txt", 10)
        try FileManager.default.createDirectory(atPath: root + "/dir", withIntermediateDirectories: false)
        XCTAssertEqual(symlink("file.txt", root + "/link"), 0, "建符号链接")

        let b = LocalFileBackend(initialPath: root)
        let entries = try b.list(root)
        let byName = Dictionary(uniqueKeysWithValues: entries.map { ($0.name, $0) })
        XCTAssertEqual(byName["file.txt"]?.kind, .file)
        XCTAssertEqual(byName["file.txt"]?.size, 10)
        XCTAssertEqual(byName["dir"]?.kind, .directory)
        XCTAssertEqual(byName["dir"]?.isDirectory, true)
        XCTAssertEqual(byName["link"]?.kind, .symlink, "符号链接报 symlink，不折叠成 file")
    }

    func testLocalMkdirRenameChmodRemove() throws {
        let root = try tmpDir()
        let b = LocalFileBackend(initialPath: root)

        try b.mkdir(root + "/new", mode: 0o700)
        XCTAssertTrue(FileManager.default.fileExists(atPath: root + "/new"))

        try touch(root + "/a.txt", 3)
        try b.rename(root + "/a.txt", root + "/b.txt")
        XCTAssertFalse(FileManager.default.fileExists(atPath: root + "/a.txt"))
        XCTAssertTrue(FileManager.default.fileExists(atPath: root + "/b.txt"))

        try b.chmod(root + "/b.txt", mode: 0o600)
        var st = stat()
        XCTAssertEqual(lstat(root + "/b.txt", &st), 0)
        XCTAssertEqual(st.st_mode & 0o777, 0o600, "chmod 真的改了模式位")

        // 非递归删空目录成功；非空目录失败。
        try b.remove(root + "/new", recursive: false)

        try FileManager.default.createDirectory(atPath: root + "/tree/sub", withIntermediateDirectories: true)
        try touch(root + "/tree/sub/x", 1)
        XCTAssertThrowsError(try b.remove(root + "/tree", recursive: false),
                            "非递归删非空目录必须失败（不折叠成成功）")
        try b.remove(root + "/tree", recursive: true)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root + "/tree"))
    }

    func testLocalWalkRespectsDepth() throws {
        let root = try tmpDir()
        try FileManager.default.createDirectory(atPath: root + "/d1/d2", withIntermediateDirectories: true)
        try touch(root + "/top.txt", 1)
        try touch(root + "/d1/f1", 1)
        try touch(root + "/d1/d2/f2", 1)

        let b = LocalFileBackend(initialPath: root)
        var deep = 0
        try b.walk(root: root, maxDepth: -1) { _ in deep += 1; return true }
        XCTAssertEqual(deep, 5, "无限深度：d1 / d1/d2 / d1/d2/f2 / d1/f1 / top.txt")

        var shallow = 0
        try b.walk(root: root, maxDepth: 1) { _ in shallow += 1; return true }
        XCTAssertEqual(shallow, 2, "maxDepth=1 只见顶层两项（d1 / top.txt）")

        var stopped = 0
        try b.walk(root: root, maxDepth: -1) { _ in stopped += 1; return stopped < 2 }
        XCTAssertEqual(stopped, 2, "visitor 返回 false 即停")
    }

    // MARK: - FilesPanelModel

    @MainActor
    func testPanelNavigateFilterSortHidden() throws {
        let root = try tmpDir()
        try FileManager.default.createDirectory(atPath: root + "/alpha", withIntermediateDirectories: false)
        try touch(root + "/beta.txt", 100)
        try touch(root + "/.hidden", 1)

        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.navigate(to: root)
        XCTAssertTrue(wait { !model.entries.isEmpty })
        XCTAssertEqual(model.entries.count, 3, "列到 3 项（含隐藏）")

        // 默认不显示隐藏文件。
        XCTAssertFalse(model.visibleEntries.contains { $0.name == ".hidden" })
        model.showHidden = true
        XCTAssertTrue(model.visibleEntries.contains { $0.name == ".hidden" })

        // 目录优先。
        XCTAssertEqual(model.visibleEntries.first?.name, "alpha")

        // 过滤。
        model.filter = "beta"
        XCTAssertEqual(model.visibleEntries.map(\.name), ["beta.txt"])

        // 排序：按大小降序（目录先）。
        model.filter = ""
        model.sortKey = .size
        model.ascending = false
        let vis = model.visibleEntries
        XCTAssertEqual(vis.first?.name, "alpha")
        XCTAssertEqual(vis.dropFirst().first?.name, "beta.txt")
    }

    @MainActor
    func testPanelNewFolderRenameDelete() throws {
        let root = try tmpDir()
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.navigate(to: root)
        XCTAssertTrue(wait { model.errorText == nil })

        model.newFolder(name: "made")
        XCTAssertTrue(wait { FileManager.default.fileExists(atPath: root + "/made") },
                      "新建文件夹落到磁盘")

        try touch(root + "/orig.txt", 5)
        model.refresh()
        XCTAssertTrue(wait { model.entries.contains { $0.name == "orig.txt" } })
        guard let orig = model.entries.first(where: { $0.name == "orig.txt" }) else { return }
        model.rename(orig, to: "renamed.txt")
        XCTAssertTrue(wait { FileManager.default.fileExists(atPath: root + "/renamed.txt") })

        model.refresh()
        XCTAssertTrue(wait { model.entries.contains { $0.name == "renamed.txt" } })
        guard let ren = model.entries.first(where: { $0.name == "renamed.txt" }) else { return }
        model.delete([ren])
        XCTAssertTrue(wait { !FileManager.default.fileExists(atPath: root + "/renamed.txt") },
                      "删除落到磁盘")
    }

    // MARK: - 面板接入（真 AppModel + 本地 shell）

    @MainActor
    func testToggleFilesPanelOnLocalShell() throws {
        let dir = try tmpDir()
        let cfg = dir + "/config"
        try "Host web\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")
        model.connect(to: .localShell)
        guard let pane = model.tabs.activeTab?.activePane else { return XCTFail("应有窗格") }
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && pane.childState != .idle {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertNotNil(pane.currentDirectory, "本地 shell 应查出内核 cwd")

        model.toggleFilesPanel()
        guard let tab = model.tabs.activeTab else { return XCTFail("应有标签页") }
        defer { model.tabs.removeAll() }
        XCTAssertTrue(tab.filesPanelVisible, "⌘⇧E 后面板应可见")
        XCTAssertNotNil(tab.filesPanel, "本地 shell 应立即可建本地后端")
        guard let panel = tab.filesPanel else { return }
        XCTAssertTrue(wait { !panel.entries.isEmpty }, "面板应列出目录")
        XCTAssertEqual(panel.currentPath, pane.currentDirectory,
                       "面板初始目录应跟随终端 cwd")

        model.toggleFilesPanel()
        XCTAssertFalse(tab.filesPanelVisible, "再切换应收起")
    }

    @MainActor
    func testRecursiveChmod() throws {
        let root = try tmpDir()
        try FileManager.default.createDirectory(atPath: root + "/d/sub",
                                                withIntermediateDirectories: true)
        try touch(root + "/d/sub/f", 1)
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.navigate(to: root)
        XCTAssertTrue(wait { model.entries.contains { $0.name == "d" } })
        guard let d = model.entries.first(where: { $0.name == "d" }) else { return XCTFail() }
        model.setPermissions(d, mode: 0o700, recursive: true)
        XCTAssertTrue(wait {
            var st = stat(); lstat(root + "/d/sub/f", &st); return (st.st_mode & 0o777) == 0o700
        }, "递归 chmod 应改到子项")
    }

    @MainActor
    func testBatchDelete() throws {
        let root = try tmpDir()
        try touch(root + "/a", 1); try touch(root + "/b", 2); try touch(root + "/c", 3)
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.navigate(to: root)
        XCTAssertTrue(wait { model.entries.count == 3 })
        let all = model.entries
        model.deleteAll(all)
        XCTAssertTrue(wait { (try? FileManager.default.contentsOfDirectory(atPath: root))?.isEmpty == true },
                      "批量删除应清空目录")
    }

    @MainActor
    func testTreeLazyLoadDirsOnly() throws {
        let root = try tmpDir()
        try FileManager.default.createDirectory(atPath: root + "/d1/d1a", withIntermediateDirectories: true)
        try FileManager.default.createDirectory(atPath: root + "/d2", withIntermediateDirectories: true)
        try touch(root + "/top.txt", 1)
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.navigate(to: root)
        XCTAssertTrue(wait { !model.entries.isEmpty })

        model.viewMode = .tree
        model.loadTreeChildren(root)
        XCTAssertTrue(wait { model.treeChildren[root] != nil }, "树根应懒加载")
        XCTAssertEqual(model.treeChildrenVisible(root).map(\.name), ["d1", "d2"],
                       "树只显示目录（不含 top.txt）")
        // 未展开前不加载子目录的孩子。
        XCTAssertNil(model.treeChildren[root + "/d1"], "未展开不预取")
        model.toggleTree(root + "/d1")
        XCTAssertTrue(wait { model.treeChildren[root + "/d1"]?.map(\.name) == ["d1a"] },
                      "展开后加载其子目录")
        model.toggleTree(root + "/d1")
        XCTAssertFalse(model.expandedDirs.contains(root + "/d1"), "再切一次收起")
    }

    @MainActor
    func testFollowTerminalCwd() throws {
        let root = try tmpDir()
        try FileManager.default.createDirectory(atPath: root + "/sub",
                                                withIntermediateDirectories: true)
        var cwd: String? = root
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.cwdProvider = { cwd }
        model.followsTerminalCwd = true
        model.start()
        XCTAssertTrue(wait { model.currentPath == root }, "初始在 root")

        // 终端 cd 到子目录 → 面板跟随。
        cwd = root + "/sub"
        model.followTick()
        XCTAssertTrue(wait { model.currentPath == root + "/sub" }, "跟随终端目录应导航过去")

        // 关掉跟随 → 不再跟。
        model.followsTerminalCwd = false
        cwd = root
        model.followTick()
        // navigate 是异步的：等一拍让「若有导航」落实，再断言它确实没动
        // （否则这条负向断言会被异步时序掩盖——变异不会红）。
        settle(0.3)
        XCTAssertEqual(model.currentPath, root + "/sub", "关掉跟随就不该动")
    }

    func testPanelKeymap() {
        XCTAssertEqual(PanelKeymap.action(key: "n", command: true, shift: true), .newFolder)
        XCTAssertEqual(PanelKeymap.action(key: "N", command: true, shift: true), .newFolder)
        XCTAssertNil(PanelKeymap.action(key: "n", command: true, shift: false),
                     "⌘N（无 ⇧）不是新建文件夹")
        XCTAssertEqual(PanelKeymap.action(key: "r", command: true, shift: false), .rename)
        XCTAssertEqual(PanelKeymap.action(key: "f", command: true, shift: false), .search)
        XCTAssertEqual(PanelKeymap.action(key: "i", command: true, shift: false), .info)
        XCTAssertEqual(PanelKeymap.action(key: "\u{8}", command: false, shift: false), .delete)
        XCTAssertNil(PanelKeymap.action(key: "x", command: true, shift: false), "未知键不拦")
        XCTAssertNil(PanelKeymap.action(key: "r", command: false, shift: false),
                     "无修饰的字母不拦（留给输入）")
    }

    // MARK: - 双栏会话（S5）

    @MainActor
    func testDualPaneSessionStartsBothPanes() throws {
        let root = try tmpDir()
        try touch(root + "/a.txt", 3)
        let remote = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        let local = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        let tab = TerminalTab(kind: .localFiles)
        tab.attachFileWorkspace(local: local, remote: remote)
        XCTAssertTrue(wait { !local.entries.isEmpty }, "本地栏应列出目录")
        XCTAssertTrue(wait { !remote.entries.isEmpty }, "远程栏应列出目录")
        XCTAssertEqual(tab.workspaceLayout?.paneIDs.count, 2)
    }



    func testGlobMatches() {
        XCTAssertTrue(SFTPFileBackend.matches("README.md", "readme"), "无通配符=子串不敏感")
        XCTAssertTrue(SFTPFileBackend.matches("a.txt", "*.txt"))
        XCTAssertFalse(SFTPFileBackend.matches("a.txt", "*.log"))
        XCTAssertTrue(SFTPFileBackend.matches("abc", "a?c"))
        XCTAssertFalse(SFTPFileBackend.matches("ac", "a?c"))
    }

    // MARK: - helpers

    /// 跑一会儿 runloop（让排队的主线程任务落实），但不断言某条件。
    @MainActor
    private func settle(_ seconds: TimeInterval) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    @MainActor
    private func wait(timeout: TimeInterval = 5, _ p: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if p() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return false
    }
}

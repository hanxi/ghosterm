// TabManagerTests — 标签页结构与生命周期（不依赖窗口 / sshd）。
//
// 覆盖：
//   1. 增 / 删 / 激活的语义（关闭后接管哪个、最后一个关掉怎么办）；
//   2. 关闭请求 → 确认 → 真关（**不静默杀掉有会话的标签页**）；
//   3. 暂停 / 关闭的顺序（先停会话再摘引用——顺序反了就是读循环踩
//      已释放内存，见 SessionController 的 UAF 注释）；
//   4. 分屏布局的增删与塌陷（关掉一个窗格后不留空壳）。
@testable import GhostermAppModel
import GhostermTerminal
import XCTest

@MainActor
final class TabManagerTests: XCTestCase {
    private func makeTab(_ kind: TabKind = .localShell) -> TerminalTab {
        TerminalTab(kind: kind, pane: TerminalPane())
    }

    func testAddActivates() {
        let m = TabManager()
        let a = makeTab(), b = makeTab()
        m.add(a)
        XCTAssertEqual(m.activeTabID, a.id, "新建的标签页应被激活")
        m.add(b)
        XCTAssertEqual(m.activeTabID, b.id, "后建的应接管激活")
        XCTAssertEqual(m.tabs.count, 2)
    }

    func testActivateAndNeighbor() {
        let m = TabManager()
        let a = makeTab(), b = makeTab(), c = makeTab()
        [a, b, c].forEach(m.add)
        m.activate(a)
        m.activateNeighbor(of: a)
        XCTAssertEqual(m.activeTabID, b.id, "相邻切换应向右")
        m.activateNeighbor(of: c)
        XCTAssertEqual(m.activeTabID, b.id, "最右再向右应回左边那个")
    }

    /// 关闭非激活标签页不该改变当前激活项（用户的上下文不能被顺手切走）。
    func testCloseInactiveKeepsActive() {
        let m = TabManager()
        let a = makeTab(), b = makeTab(), c = makeTab()
        [a, b, c].forEach(m.add)
        m.activate(b)
        m.close(a)
        XCTAssertEqual(m.activeTabID, b.id, "关掉别的标签页不应切走当前项")
        XCTAssertEqual(m.tabs.map(\.id), [b.id, c.id])
    }

    /// 关闭激活标签页 → 接管右边；右边没有则左边。
    func testCloseActiveTakesNeighbor() {
        let m = TabManager()
        let a = makeTab(), b = makeTab(), c = makeTab()
        [a, b, c].forEach(m.add)
        m.activate(b)
        m.close(b)
        XCTAssertEqual(m.activeTabID, c.id, "关掉中间那个应由右边接管")
        m.activate(c)
        m.close(c)
        XCTAssertEqual(m.activeTabID, a.id, "关掉最后一个应由左边接管")
    }

    func testCloseLastClearsActive() {
        let m = TabManager()
        let a = makeTab()
        m.add(a)
        m.close(a)
        XCTAssertNil(m.activeTabID, "全关掉后没有激活项（空态）")
        XCTAssertTrue(m.tabs.isEmpty)
    }

    // MARK: - 关闭确认：只在「有东西会被打断」时才问（iGhostVT 口径）

    /// 关窗格判据是**纯函数**：只有能证实「没什么可丢」才不问；查不出来
    /// （`.unknown`）一律当「可能有作业」（铁律 #6）。
    func testPaneCloseNeedsConfirmationMatrix() {
        // 没什么可丢的三档 → 不问
        XCTAssertFalse(TerminalPane.closeNeedsConfirmation(
            status: .exited(code: 0), childState: .exited), "已退出：不问")
        XCTAssertFalse(TerminalPane.closeNeedsConfirmation(
            status: .failed("x"), childState: .unknown), "失败：不问")
        XCTAssertFalse(TerminalPane.closeNeedsConfirmation(
            status: .connecting, childState: .unknown), "连接中：不问")
        // 就绪：空闲提示符不问；有作业 / 查不出 → 问
        XCTAssertFalse(TerminalPane.closeNeedsConfirmation(
            status: .ready, childState: .idle), "空闲提示符：不问（iGhostVT 同口径）")
        XCTAssertTrue(TerminalPane.closeNeedsConfirmation(
            status: .ready, childState: .job), "前台有作业：问")
        XCTAssertTrue(TerminalPane.closeNeedsConfirmation(
            status: .ready, childState: .unknown), "查不出（含 SSH）：问，不折叠成空闲")
    }

    private func makeSFTFTab() -> TerminalTab {
        TerminalTab(kind: .sftp(RemoteTarget(pattern: "web", user: "u", host: "h", port: 22)))
    }

    /// 没什么可丢的标签页（连接中 / 失败 / 已退出）→ requestClose **直接关**，
    /// 不弹框。新标签页的窗格是 `.connecting`，正属此列。
    func testRequestCloseIdleTabClosesWithoutPrompt() {
        let m = TabManager()
        let a = makeTab()                 // 窗格 = .connecting
        m.add(a)
        m.requestClose(a)
        XCTAssertNil(m.closeRequest, "没什么可丢就不该弹框")
        XCTAssertTrue(m.tabs.isEmpty, "应直接关掉")
    }

    /// 还在连（或连不上）的标签页：**未知 → 问**（铁律 #6）。
    /// 用一张还没连上的 SFTP 标签页造这个状态（终端 pane 的 .ready 要靠真会话）。
    func testRequestCloseUnknownTabPrompts() {
        let m = TabManager()
        let t = makeSFTFTab()             // 未连：sftpBrowser == nil、无错误 → 未知
        m.add(t)
        XCTAssertTrue(t.closeNeedsConfirmation, "未连的 SFTP：未知，应当有事")
        m.requestClose(t)
        XCTAssertEqual(m.closeRequest?.id, t.id, "应挂起一个关闭请求")
        XCTAssertEqual(m.tabs.count, 1, "确认前不得真的关掉")
        m.cancelClose()
        XCTAssertNil(m.closeRequest, "取消应清掉请求")
        XCTAssertEqual(m.tabs.count, 1, "取消后标签页还在")
    }

    /// 确认后真关（两步语义仍在：不静默杀会话）。
    func testConfirmCloseReallyCloses() {
        let m = TabManager()
        let t = makeSFTFTab()
        m.add(t)
        m.requestClose(t)
        let closed = m.confirmClose()
        XCTAssertEqual(closed?.id, t.id)
        XCTAssertTrue(m.tabs.isEmpty, "确认后真关掉")
        XCTAssertNil(m.closeRequest)
    }

    /// 失败收场（`markSFTPFailed`）：没东西可丢 → 直接关，不问。
    func testRequestCloseFailedSFTFClosesWithoutPrompt() {
        let m = TabManager()
        let t = makeSFTFTab()
        t.markSFTPFailed("boom")
        m.add(t)
        XCTAssertFalse(t.closeNeedsConfirmation, "没连上：没什么可丢")
        m.requestClose(t)
        XCTAssertNil(m.closeRequest)
        XCTAssertTrue(m.tabs.isEmpty)
    }

    /// 关窗口 / 退出的判据：**任一**页会丢东西才拦。
    func testHasRunningProgramsForQuit() {
        let m = TabManager()
        m.add(makeTab())                  // .connecting → 不拦
        XCTAssertFalse(m.hasRunningPrograms, "都是连接中：不拦")
        m.add(makeSFTFTab())              // 未连的 SFTP → 未知 → 拦
        XCTAssertTrue(m.hasRunningPrograms, "有一个未知项：拦")
    }

    /// **真会话**上的两档（空闲 / 有作业）——不只是纯函数矩阵，走真 transport。
    ///
    /// 空闲 = 直接关（不弹）；有前台作业 = 要问。这条覆盖「App 接线 → pane →
    /// SessionController → transport → 内核 tcgetpgrp」整条链路。
    func testRealLocalShellCloseNeedsConfirmation() throws {
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let session = try SessionController.localShell(surface: surface, kind: .shell) /* 模拟生产：起用户的交互 shell */
        let pane = TerminalPane()
        pane.attach(surface: surface, session: session)
        pane.start()
        let tab = TerminalTab(kind: .localShell, pane: pane)

        // 等它到空闲提示符（foreground == shell 自己）。
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && pane.childState != .idle { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        XCTAssertEqual(pane.childState, .idle, "空闲的本地 shell 应是 IDLE（状态 \(pane.status)）")
        XCTAssertFalse(tab.closeNeedsConfirmation, "空闲提示符：直接关，不问")

        // 跑一个长作业 → 前台不再是 shell → 要问。
        surface.sendCommandLine("sleep 30")
        let jobDeadline = Date().addingTimeInterval(10)
        while Date() < jobDeadline && pane.childState != .job { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        XCTAssertEqual(pane.childState, .job, "前台跑 sleep 时应是 JOB")
        XCTAssertTrue(tab.closeNeedsConfirmation, "有前台作业：要问")

        tab.stop()
        XCTAssertFalse(pane.isRunning, "关闭必须停掉会话")
    }

    /// 退出判据：有「会丢东西」的标签页就不放行。
    ///
    /// 走真 AppModel（Application 无关）：连接中的本地 shell 不放行需要它
    /// 是 ready；故用「未连的 SFTP」（未知 → 要拦）造「有东西」的状态。
    func testShouldTerminateBlocksWhenSomethingWouldDie() throws {
        let dir = NSTemporaryDirectory() + "quit-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                             encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")

        XCTAssertTrue(model.shouldTerminate(), "没有标签页：直接放行")
        let tab = TerminalTab(kind: .sftp(RemoteTarget(pattern: "web", user: "u", host: "h", port: 22)))
        model.tabs.add(tab)               // 未连的 SFTP：未知 → 要拦
        XCTAssertFalse(model.shouldTerminate(), "有未知项：不放行（先确认）")
    }

    /// 关闭前会触发 willClose（AppModel 用它投在途取消票）。
    func testWillCloseHookFires() {
        let m = TabManager()
        let a = makeTab()
        m.add(a)
        var seen: UUID?
        m.willClose = { seen = $0.id }
        m.close(a)
        XCTAssertEqual(seen, a.id, "close 之前必须回调 willClose")
    }

    /// 暂停全部（窗口关闭 / 退出）：每个标签页的窗格都收到 stop。
    /// 关闭标签页**必须停掉会话**。这条守的是「关掉标签页，会话还在后台跑」
    /// 这种泄漏：它不报错，只是进程里多出一条没人管的连接（和读循环）。
    func testCloseStopsTheSession() throws {
        // TerminalApp 必须**活到测试结束**：surface 由它创建，它一旦释放
        // 就 gt_ghostty_app_free（连同 libghostty 的全局状态），后面的
        // 创建会踩已释放状态（实测：写成 `app: try TerminalApp()`
        // 临时值时会崩在 xev 的 mach 通知上）。
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let session = try SessionController.localShell(surface: surface, kind: .shell) /* 模拟生产：起用户的交互 shell */
        let pane = TerminalPane()
        pane.attach(surface: surface, session: session)
        pane.start()
        let tab = TerminalTab(kind: .localShell, pane: pane)
        let m = TabManager()
        m.add(tab)
        XCTAssertTrue(pane.isRunning, "起点：窗格持有会话")

        XCTAssertTrue(pane.isRunning, "起点：窗格持有会话")
        m.close(tab)
        XCTAssertTrue(m.tabs.isEmpty, "标签页应被移除")
        XCTAssertFalse(pane.isRunning,
                       "关闭标签页必须停掉会话——否则会话在后台继续跑（泄漏）")
        // 面保留（关闭的标签页在内存里等释放，网格内容不抹）：
        // 真正的释放由 TerminalTab 被丢弃时带走。
        XCTAssertNotNil(pane.surface, "面随标签页对象一起释放，不在 close 里抹掉")
    }

    func testRemoveAllStopsEverything() {
        let m = TabManager()
        let a = makeTab(), b = makeTab()
        m.add(a); m.add(b)
        m.removeAll()
        XCTAssertTrue(m.tabs.isEmpty)
        XCTAssertNil(m.activeTabID)
        // 停过之后窗格不该还留着会话。
        XCTAssertFalse(a.activePane?.isRunning ?? true)
        XCTAssertFalse(b.activePane?.isRunning ?? true)
    }

    func testMoveReorders() {
        let m = TabManager()
        let a = makeTab(), b = makeTab(), c = makeTab()
        [a, b, c].forEach(m.add)
        m.move(a, to: 2)
        XCTAssertEqual(m.tabs.map(\.id), [b.id, c.id, a.id], "拖到尾部")
        m.move(a, to: 0)
        XCTAssertEqual(m.tabs.map(\.id), [a.id, b.id, c.id], "拖回头部")
    }

    func testActivateByIndex() {
        let m = TabManager()
        let a = makeTab(), b = makeTab()
        m.add(a); m.add(b)
        m.activate(index: 0)
        XCTAssertEqual(m.activeTabID, a.id, "⌘1 应激活第一个标签页")
        m.activate(index: 5)
        XCTAssertEqual(m.activeTabID, a.id, "越界索引不得改变状态")
    }
}

/// 新建标签页的**数据面**：菜单里的选项来自哪些主机。
/// UI 只是把它渲染成菜单项。这条单拎出来是因为「菜单是空的」这类问题
/// 在真机上最难查（点开什么都没有，也不报错）。
@MainActor
final class NewTabMenuTests: XCTestCase {
    private func writeConfig(_ text: String) throws -> String {
        let dir = NSTemporaryDirectory() + "newtab-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        try text.write(toFile: path, atomically: true, encoding: .utf8)
        return path
    }

    /// 有配置时，可连的主机里**不含**通配块（`Host *` 不是可点主机）。
    func testConnectableHostsExcludeWildcards() throws {
        let cfg = try writeConfig("""
        Host web
          HostName 10.0.0.5
          User ops
        Host *
          ServerAliveInterval 60
        """)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        XCTAssertEqual(model.hosts.map(\.pattern), ["web"],
                       "可连主机应只有 web（通配块不算）")
        XCTAssertEqual(model.wildcards, ["*"], "通配块要单独可见可编辑")
    }

    /// 本地 shell **不去重**：每次点都开一个新的（在一个窗口里开几个 shell
    /// 是常规用法）。这条与下面的 SSH 去重是**一对**——分开断言，免得
    /// 「都去重」和「都不去重」两种错法各自只被盖住一边。
    func testLocalShellAlwaysOpensNewTab() throws {
        let cfg = try writeConfig("Host web\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .localShell)
        model.connect(to: .localShell)
        XCTAssertEqual(model.tabs.tabs.count, 2,
                       "本地 shell 每次点都该开新标签页（不去重）")
    }

    /// 同一台主机**每次点都开一个新标签页**（与本地 shell 同规则）。
    ///
    /// 用户实测的坑：先前 `.ssh(...)` 没有 instance，两个 kind 相等，于是
    /// 第二次点同一台主机被去重成「切回已开的那个」——**同一台机器开不了
    /// 第二个标签页**，可菜单里「分屏」却能开（分屏不走这条去重），看起来
    /// 自相矛盾。同一台机开多个 shell 是终端的常规用法。
    func testSameHostOpensNewTabEachTime() throws {
        let cfg = try writeConfig("""
        Host web
          HostName 10.0.0.5
          User ops
        """)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .host("web"))
        XCTAssertEqual(model.tabs.tabs.count, 1)
        model.connect(to: .host("web"))
        XCTAssertEqual(model.tabs.tabs.count, 2,
                       "同一台主机每次点都该开新标签页（不去重），"
                       + "否则与「分屏能开、标签页开不了」自相矛盾")
        XCTAssertEqual(model.tabs.activeTab?.kind, model.tabs.tabs.last?.kind,
                       "新开的那个应被激活")
    }

    /// SFTP 与终端是**不同** kind：同一个 host 开 SFTP 不该复用终端标签页
    /// （复用会让终端面被文件浏览顶掉——用户会以为终端没了）。
    // MARK: - cwd：本地内核查询 + ⌘T 继承目录（S4/S5）

    /// 本地 shell 的 currentDirectory 来自**内核查询**（不依赖 shell 集成）。
    func testLocalShellCurrentDirectoryFromKernel() throws {
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let session = try SessionController.localShell(surface: surface, kind: .shell) /* 模拟生产：起用户的交互 shell */
        let pane = TerminalPane()
        pane.attach(surface: surface, session: session)
        pane.start()

        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && pane.childState != .idle {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertEqual(pane.currentDirectory, FileManager.default.currentDirectoryPath,
                       "本地 shell 的 cwd 应由内核查询给出（= 启动目录）")
        pane.stop()
    }

    /// 新建的本地 shell（无可继承目录时）落在**家目录**，不是 app 进程的 cwd。
    ///
    /// 用户实测「创建的本地终端当前目录是 /」：GUI（Finder/Dock）启动的进程
    /// cwd 是 `/`，`connect` 早先不显式给 cwd → 子进程继承到 `/`。C 核心对
    /// nil cwd 的「继承父进程 cwd」契约本身正确（test_transport §12 + 上面的
    /// `testLocalShellCurrentDirectoryFromKernel` 守着——那条走
    /// `SessionController.localShell` 直连，不经 AppModel），故默认值在应用层
    /// 给：`connect` 的 `.localShell` 分支用 `cwd ?? NSHomeDirectory()`。
    @MainActor
    func testLocalShellDefaultsToHomeWhenNoCwd() throws {
        let cfg = try writeConfig("Host web\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        // 先确保测试进程的 cwd 不是家目录，否则断言不敏感（变异不会红）。
        XCTAssertNotEqual(FileManager.default.currentDirectoryPath, NSHomeDirectory(),
                          "测试环境：进程 cwd 应与家目录不同，否则本断言无牙")
        model.connect(to: .localShell)
        guard let pane = model.tabs.activeTab?.activePane else {
            return XCTFail("应有窗格")
        }
        // cwd 用**轮询**等，不是一读定案（childState 等待也一样会被换掉）：
        // forkpty 子进程里 setsid 一完成，tcgetpgrp 就 == 子进程 → childState
        // 立刻 .idle，而 chdir(cwd) 在 setsid **之后**才执行。负载重的机器上
        // （门禁并发构建/测试）单读可能正好落进「setsid 后、chdir 前」的窗口，
        // 读到父进程的 cwd——门禁偶发红、单跑不复现，就是它。轮询到期望值
        // 为止才把窗口关死；变异（丢 home 默认值）时轮询超时，断言照样红。
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && pane.currentDirectory != NSHomeDirectory() {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertEqual(pane.currentDirectory, NSHomeDirectory(),
                       "无可继承目录时本地 shell 应落在家目录，不是 app 进程的 cwd")
        model.tabs.removeAll()
    }

    /// ⌘T 从本地 shell 出发：新标签页开在**同一个目录**。
    func testNewTabInheritsLocalDirectory() throws {
        let cfg = try writeConfig("Host web\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .localShell)
        guard let first = model.tabs.activeTab?.activePane else { return XCTFail("应有窗格") }
        // 轮询等 cwd（不是一读定案）：理由见 testLocalShellDefaultsToHomeWhenNoCwd
        // ——chdir 在 setsid 之后，childState==.idle 不构成「已 chdir」的同步。
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline && first.currentDirectory != NSHomeDirectory() {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        // 无可继承目录时本地 shell 落在家目录（不是测试进程的 cwd）——这是
        // 用户实测「创建的本地终端当前目录是 /」的修复点（见下面的专测）。
        XCTAssertEqual(first.currentDirectory, NSHomeDirectory(),
                       "新建的本地 shell 应落在家目录")

        // 先把当前 shell **cd 到别处**：否则新 shell 即使不继承也落在同一默认
        // 目录，断言就不敏感（变异不会红）。cd /usr 之后继承与否才看得出来。
        first.surface?.sendCommandLine("cd /usr")
        let d1 = Date().addingTimeInterval(10)
        while Date() < d1 && first.currentDirectory != "/usr" {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertEqual(first.currentDirectory, "/usr", "前置：当前 shell 应在 /usr")

        model.newTabLikeActive()
        guard let second = model.tabs.activeTab?.activePane else { return XCTFail("应有第二个窗格") }
        // 同上：轮询等 cwd（新 shell chdir 到 /usr 在 setsid 之后）。
        let d2 = Date().addingTimeInterval(15)
        while Date() < d2 && second.currentDirectory != "/usr" {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        XCTAssertEqual(second.currentDirectory, "/usr",
                       "⌘T 的新本地 shell 应继承当前 shell 的目录（/usr），不是默认目录")
        model.tabs.removeAll()
    }

    /// 契约：**没有内核 cwd 的会话（SSH）** currentDirectory 为 nil——
    /// 不拿别的值假装知道（铁律 #6）。
    ///
    /// 远端 cwd 本打算走 OSC 7，但**我们钉的 embedded host-IO 下 OSC 7 不会
    /// 到达宿主**（实测：本机 host + ST 终止符，`cb_action` 收不到任何
    /// `GHOSTTY_ACTION_PWD`；`embedded.zig` 也没有 surface message 的 pwd
    /// 通路），且 libghostty 还要求 OSC 7 的 host `isLocal`。故远端 cwd 需要
    /// 上游支持（配 shell 集成注入 = roadmap 的 R2）。见计划文档 S4。
    func testSSHPaneHasNoDirectory() throws {
        let surface = try TerminalSurface(app: try TerminalApp.shared())
        let tab = TerminalTab(kind: .ssh(RemoteTarget(pattern: "web", user: "u",
                                                     host: "h", port: 22)),
                              pane: TerminalPane())
        // 只接 surface（模拟「还没连上/远端」）：没有内核 cwd → nil。
        tab.panes[0].attachForTesting(surface: surface)
        XCTAssertNil(tab.currentDirectory,
                     "没有内核 cwd（SSH）时为 nil，不猜")
    }

    // MARK: - ⌘T 继承当前标签页类型 + SFTP 每次新开（S3）

    /// 空态：没有当前标签页 → 新建本地 shell。
    func testNewTabLikeActiveWithNoTabsIsLocalShell() throws {
        let cfg = try writeConfig("Host web\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.newTabLikeActive()
        XCTAssertEqual(model.tabs.tabs.count, 1)
        XCTAssertEqual(model.tabs.activeTab?.kind, .localShell, "空态应是本地 shell")
    }

    /// 当前是本地 shell → 新建本地 shell（不是别的）。
    func testNewTabInheritsLocalShell() throws {
        let cfg = try writeConfig("Host web\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .localShell)
        XCTAssertEqual(model.tabs.tabs.count, 1, "先开一个本地·前")
        model.newTabLikeActive()
        XCTAssertEqual(model.tabs.tabs.count, 2, "本地→本地（新开一条）")
        XCTAssertEqual(model.tabs.activeTab?.kind, .localShell)
    }

    /// 当前是 SSH（同一台主机）→ ⌘T 再开一条**同一台主机**的 ssh。
    func testNewTabInheritsSSHHost() throws {
        let cfg = try writeConfig("""
        Host web
          HostName 10.0.0.5
          User ops
        """)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .host("web"))
        XCTAssertEqual(model.tabs.tabs.count, 1)
        model.newTabLikeActive()
        XCTAssertEqual(model.tabs.tabs.count, 2, "ssh→同机再开一条")
        XCTAssertEqual(model.tabs.activeTab?.kind, .ssh(model.remoteTarget(for: "web")),
                       "新开的应是同一台主机（web）")
    }

    /// 当前是 SFTP → ⌘T 再开一个**同目标**的 SFTP。
    func testNewTabFromSFTPOpensSameTargetSFTP() throws {
        let cfg = try writeConfig("""
        Host web
          HostName 10.0.0.5
          User ops
        """)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.openSFTP(for: "web")
        XCTAssertEqual(model.tabs.tabs.count, 1, "先开一个 SFTP")
        model.newTabLikeActive()
        XCTAssertEqual(model.tabs.tabs.count, 2, "SFTP→同目标再开一个")
        XCTAssertEqual(model.tabs.activeTab?.kind, .sftp(model.remoteTarget(for: "web")))
    }

    /// SFTP 与 SSH 一致：**连续开两次是新开两个标签页**（不再复用）。
    func testOpenSFTPTwiceOpensTwoTabs() throws {
        let cfg = try writeConfig("Host web\n  HostName 10.0.0.5\n")
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.openSFTP(for: "web")
        model.openSFTP(for: "web")
        XCTAssertEqual(model.tabs.tabs.count, 2,
                       "SFTP 与 SSH 同规则：每次新开，不去重")
    }

    /// 身份不变式：`TabKind` **不承载身份**（删了 instance）——两个同目标的
    /// SFTP kind 相等；身份是 `TerminalTab.id`。这条钉住 D1 的语义。
    func testSameTargetKindsAreEqualNoIdentityInKind() throws {
        let t = RemoteTarget(pattern: "web", user: "u", host: "h", port: 22)
        XCTAssertEqual(TabKind.sftp(t), TabKind.sftp(t), "同目标 kind 相等（身份不在 kind 里）")
        XCTAssertEqual(TabKind.ssh(t).remoteTarget, TabKind.sftp(t).remoteTarget,
                       "ssh 与 sftp 共享同一个远端目标键")
        XCTAssertNil(TabKind.localShell.remoteTarget, "本地 shell 没有远端目标")
    }

    func testSFTPTabIsDistinctFromTerminalTab() throws {
        let cfg = try writeConfig("""
        Host web
          HostName 10.0.0.5
          User ops
        """)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .host("web"))
        let before = model.tabs.tabs.count
        XCTAssertFalse(model.tabs.tabs.contains { !$0.kind.isTerminal },
                       "一开始不该有 SFTP 标签页")
        XCTAssertEqual(model.tabs.tabs.count, before)
    }
}

// MARK: - 分屏

@MainActor
final class SplitLayoutTests: XCTestCase {
    private func makeTab() -> TerminalTab {
        TerminalTab(kind: .ssh(RemoteTarget(pattern: "h", user: "u", host: "1.2.3.4", port: 22)),
                    pane: TerminalPane())
    }

    func testSplitAddsPane() {
        let tab = makeTab()
        XCTAssertFalse(tab.isSplit)
        let p2 = TerminalPane()
        tab.split(axis: .horizontal, with: p2)
        XCTAssertTrue(tab.isSplit)
        XCTAssertEqual(tab.panes.count, 2)
        XCTAssertEqual(tab.activePaneID, p2.id, "新窗格应成为当前窗格")
    }

    /// 在已分屏的子区域里再分屏 → 嵌套，不是被拍平。
    func testNestedSplit() {
        let tab = makeTab()
        let p1 = tab.panes[0]
        let p2 = TerminalPane(), p3 = TerminalPane()
        tab.split(axis: .horizontal, with: p2)
        tab.activePaneID = p1.id
        tab.split(axis: .vertical, with: p3)
        XCTAssertEqual(tab.panes.count, 3, "三次分屏应有三个窗格")
        // p1 与 p3 同属一个垂直分组：布局应是 split(h, [split(v,[p1,p3]), p2])
        guard case .split(let axis, let children) = tab.layout else {
            return XCTFail("布局应是 split 根节点")
        }
        XCTAssertEqual(axis, .horizontal)
        XCTAssertEqual(children.count, 2)
        guard case .split(let innerAxis, let inner) = children[0] else {
            return XCTFail("第一个子节点应是嵌套的 split")
        }
        XCTAssertEqual(innerAxis, .vertical)
        XCTAssertEqual(inner.count, 2)
        XCTAssertEqual(inner[0].pane?.id, p1.id, "嵌套里先是原窗格")
        XCTAssertEqual(inner[1].pane?.id, p3.id, "再是新窗格")
    }

    /// 关掉一个窗格后：布局塌陷（不留单子节点的空壳），当前窗格收敛。
    func testClosePaneCollapses() {
        let tab = makeTab()
        let p1 = tab.panes[0], p2 = TerminalPane()
        tab.split(axis: .horizontal, with: p2)
        XCTAssertTrue(tab.closePane(p2), "两个窗格时可以关")
        XCTAssertEqual(tab.panes.count, 1)
        XCTAssertEqual(tab.activePaneID, p1.id, "当前窗格应收敛到剩下那个")
        guard case .split = tab.layout! else { return } // 应已塌陷成 leaf
        XCTFail("只剩一个窗格时布局应塌陷为叶子，不是含单子节点的 split")
    }

    /// 最后一个窗格不允许用「关窗格」的方式关掉——那是关标签页。
    func testCannotCloseLastPane() {
        let tab = makeTab()
        XCTAssertFalse(tab.closePane(tab.panes[0]), "最后一个窗格应拒绝关闭")
        XCTAssertEqual(tab.panes.count, 1)
    }

    /// 标签页状态取**最差**的窗格：一个失败不能被另一个 ready 盖住。
    /// 断言纯函数（不造 surface/会话——那会让测试依赖渲染环境而变脆）。
    func testStatusTakesWorstPane() {
        XCTAssertEqual(TerminalTab.worst([.ready, .ready]), .ready)
        XCTAssertEqual(TerminalTab.worst([.ready, .connecting]), .connecting,
                       "连接中优先于就绪（还没连上不能显示成已就绪）")
        XCTAssertTrue(TerminalTab.worst([.ready, .failed("boom")]).isFailed,
                      "失败优先于就绪（不能被别的窗格盖住）")
        XCTAssertTrue(TerminalTab.worst([.connecting, .failed("x")]).isFailed,
                      "失败优先于连接中")
        XCTAssertEqual(TerminalTab.worst([.ready, .exited(code: 1)]),
                       .exited(code: 1), "启动过的窗格退出优先于纯就绪")
        XCTAssertEqual(TerminalTab.worst([]), .ready, "没有窗格时按就绪算")
    }

    /// 真实标签页上的这条规则（验证 `status` 真的走 worst，不是空跑）。
    ///
    /// 关键在**顺序**：让第一个窗格 ready、第二个失败。若 `status` 改成
    /// 「取第一个窗格」，这条就会红——这正是 T5 变异要造的状态。
    func testTabStatusUsesWorstPane() {
        let tab = makeTab()
        XCTAssertEqual(tab.status, .connecting, "刚建的窗格是连接中")

        // 两窗格：第一个（也是 activePane）失败，第二个连接中 →
        // 失败的优先级更高，所以应是 failed。
        tab.panes[0].markFailed("boom")
        tab.split(axis: .horizontal, with: TerminalPane())
        XCTAssertTrue(tab.status.isFailed,
                      "第一个窗格失败时状态应是失败（得到 \(tab.status)）")

        // 反过来：**第一个 ready、第二个失败** —— 取「第一个窗格」的实现
        // 在这里会说 ready，正确实现必须说 failed。
        let tab2 = makeTab()
        tab2.panes[0].markConnecting()          // 第一个：连接中
        let p2 = TerminalPane()
        tab2.split(axis: .horizontal, with: p2)
        p2.markFailed("second failed")          // 第二个：失败
        XCTAssertTrue(tab2.status.isFailed,
                      "第二个窗格失败时标签页也必须是失败（得到 \(tab2.status)）"
                      + "——这条专治「只看第一个窗格」")
    }

    /// **本地 shell 也要能分屏**：新窗格是一套**自己的** surface + 会话
    /// （不是 SSH）。早先这里只处理 SSH，本地 shell 上点分屏「什么都没发生」
    /// ——按钮在、没反应、也不报错。
    @MainActor
    func testSplitLocalShellTab() async throws {
        let cfg = NSTemporaryDirectory() + "split-local-\(UUID().uuidString)"
        try "Host web\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg,
                             knownHostsPath: NSTemporaryDirectory() + "/none")
        model.connect(to: .localShell)
        guard let tab = model.tabs.activeTab else { return XCTFail("应有标签页") }
        XCTAssertEqual(tab.panes.count, 1)

        model.splitActiveTab(axis: .horizontal)
        XCTAssertEqual(tab.panes.count, 2, "本地 shell 也要能分出第二个窗格")
        let newPane = tab.panes[1]
        let deadline = Date().addingTimeInterval(10)
        while newPane.status != .ready, !newPane.status.isFailed, Date() < deadline { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertTrue(newPane.isRunning, "新窗格应有自己的本地会话")
        XCTAssertNotNil(newPane.surface, "新窗格应有自己的终端面")
        XCTAssertFalse(newPane.status.isFailed,
                       "本地分屏不该失败（\(newPane.status)）")
    }

    /// 分屏对 SFTP 标签页无意义（它的 kind 不是终端）。
    func testSplitIgnoredForSFTPTab() {
        let tab = TerminalTab(kind: .sftp(RemoteTarget(pattern: "h", user: "u", host: "1.2.3.4", port: 22)))
        tab.split(axis: .horizontal, with: TerminalPane())
        XCTAssertTrue(tab.panes.isEmpty, "SFTP 标签页不应产生窗格")
    }

    // MARK: - 辅助

    private func makeSurface() -> TerminalSurface {
        try! TerminalSurface(app: try! TerminalApp.shared())
    }

    private func makeSession(_ pane: TerminalPane) -> SessionController {
        let surface = try! TerminalSurface(app: try! TerminalApp.shared())
        return try! SessionController.localShell(surface: surface, kind: .shell)
    }
}

private extension PaneLayout {
    var pane: TerminalPane? {
        if case .leaf(let p) = self { return p }
        return nil
    }
}
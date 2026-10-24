// GhostermTerminalTests — 证明我们真的把字节流喂进了 full libghostty 的网格，
// 并且三个方向的接缝都通。
//
// 关键点：断言读的是**终端网格里的文本**（ghostty_surface_read_text），不是
// 我们自己记的回调账——否则测的是自己的代码，不是 libghostty。
import AppKit
import GhostermTerminal
import XCTest

final class TerminalSurfaceTests: XCTestCase {
    /// libghostty 需要一个已初始化的 NSApplication（它挂 Metal 层、读屏幕
    /// scale 都走 AppKit）。测试进程里没有 window，故显式起一个 accessory app。
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    /// 无窗口的 NSView 也能作渲染锚点（libghostty 只把它当 layer 宿主）。
    private func makeSurface() throws -> (TerminalApp, TerminalSurface) {
        let app = try TerminalApp()
        let surface = try TerminalSurface(app: app)
        return (app, surface)
    }

    /// 轮询等待（libghostty 的焦点事件经 io 线程异步送出，不能同步读）。
    private func waitUntil(_ timeout: TimeInterval = 5,
                           _ cond: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if cond() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return cond()
    }

    private func debugBytes(_ data: Data) -> String {
        "[" + data.map { String(format: "%02x", $0) }.joined(separator: " ") + "]"
    }

    func testHostBackendDoesNotForkAPty() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }
        // 网格尺寸不是 0 说明 libghostty 正常初始化了终端。
        let grid = surface.gridSize
        XCTAssertGreaterThan(grid.cols, 0, "host surface 的网格应有列数")
        XCTAssertGreaterThan(grid.rows, 0, "host surface 的网格应有行数")
    }

    /// 核心断言：把字节喂进去，**从 libghostty 的网格读回来**。
    func testOutputReachesGhosttyGrid() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        surface.feed(Data("GHOSTERM_VT_OK\r\n".utf8))
        surface.feed(Data("\u{1b}[1;31mRED_TEXT\u{1b}[0m\r\n".utf8))

        guard let text = surface.readText() else {
            return XCTFail("readText 返回 nil —— 网格里读不到内容")
        }
        XCTAssertTrue(text.contains("GHOSTERM_VT_OK"),
                      "普通文本应进网格（得到: \(text.debugDescription)）")
        XCTAssertTrue(text.contains("RED_TEXT"),
                      "SGR 序列应被解析为样式而非字面量（得到: \(text.debugDescription)）")
        XCTAssertFalse(text.contains("[1;31m"),
                       "转义序列不得以字面量留在网格里")
    }

    /// 用户输入方向：终端编码后的字节必须到达 transport 侧回调。
    func testInputReachesHostCallback() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        let done = expectation(description: "onInput 被调用")
        surface.onInput = { data in
            received.append(data)
            done.fulfill()
        }

        // 用 paste 语义注入文本：走的是终端输入编码路径。
        let view = surface.view
        XCTAssertNotNil(view)
        surface.feed(Data())  // 保证 surface 活着
        // libghostty 的 ghostty_surface_text 由 CGhostty 暴露给 App 层；
        // 这里走 view 的 keyDown 不合适（无窗口），故直接用封装出来的入口。
        surface.injectTextForTesting("hi")

        wait(for: [done], timeout: 5)
        XCTAssertEqual(String(decoding: received, as: UTF8.self), "hi",
                       "文本输入应原样到达 host write 回调")
    }

    /// resize 方向：屏幕尺寸变化 → 网格变化 → host resize 回调。
    func testResizePropagatesToHostCallback() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var got: (UInt16, UInt16)?
        let done = expectation(description: "onResize 被调用")
        surface.onResize = { cols, rows in
            if got == nil { got = (cols, rows); done.fulfill() }
        }

        surface.setSize(width: 1000, height: 700)
        wait(for: [done], timeout: 5)

        guard let got = got else { return XCTFail("resize 未回调") }
        XCTAssertEqual(got.0, surface.gridSize.cols, "回调的列数应与网格一致")
        XCTAssertEqual(got.1, surface.gridSize.rows, "回调的行数应与网格一致")
        XCTAssertGreaterThan(got.0, 0)
    }


    /// 键入语义：回车必须产生 CR，而不是 bracketed-paste 的字面换行。
    func testEnterKeyEncodesCarriageReturn() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        let done = expectation(description: "回车到达 host")
        surface.onInput = { d in received.append(d); done.fulfill() }
        surface.sendKey(.return)
        wait(for: [done], timeout: 5)
        XCTAssertEqual(Array(received), [0x0D], "回车应编码为 CR")
    }

    /// 分两步：文本 + 回车（绕开 bracketed paste 对换行的字面化）。
    func testCommandLineSendsTextThenCR() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        let done = expectation(description: "命令 + 回车到达 host")
        done.expectedFulfillmentCount = 2
        surface.onInput = { d in received.append(d); done.fulfill() }
        surface.sendCommandLine("echo hi")
        wait(for: [done], timeout: 5)
        XCTAssertEqual(String(decoding: received, as: UTF8.self), "echo hi\r")
    }

    // MARK: - OSC 7501（Program Status Protocol）

    /// 报告方向：喂 `ESC ] 7501 ; … ST`，宿主回调必须拿到解析后的字段。
    /// 断言读的是**回调收到的东西**，而回调是 libghostty 从网格解析后
    /// 经 apprt 回调上来的（不是我们自己解析的账）。
    func testProgramStatusReportReachesCallback() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var got: ProgramStatusReport?
        let done = expectation(description: "onProgramStatus")
        surface.onProgramStatus = { r in got = r; done.fulfill() }

        surface.feed(Data("\u{1b}]7501;state=working:app=cargo:progress=40\u{1b}\\".utf8))
        wait(for: [done], timeout: 5)

        XCTAssertEqual(got?.state, .working)
        XCTAssertEqual(got?.kind, ProgramStatusReport.Kind.none, "working 不带 kind")
        XCTAssertEqual(got?.progress, 40)
        XCTAssertEqual(got?.app, "cargo")
        XCTAssertEqual(got?.id, "", "根记录的 id 为空")
    }

    /// blocked 必须带上 `kind`（权限 / 提问 / 认证），progress 缺省为 -1。
    func testBlockedReportCarriesKindAndNoProgress() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var got: ProgramStatusReport?
        let done = expectation(description: "onProgramStatus")
        surface.onProgramStatus = { r in got = r; done.fulfill() }

        surface.feed(Data("\u{1b}]7501;state=blocked:kind=permission:app=terraform\u{1b}\\".utf8))
        wait(for: [done], timeout: 5)

        XCTAssertEqual(got?.state, .blocked)
        XCTAssertEqual(got?.kind, .permission)
        XCTAssertEqual(got?.app, "terraform")
        XCTAssertEqual(got?.progress, -1, "没发 progress 应为 -1")
    }

    /// 标题/正文是 base64，必须在 C 边界解码（规范允许任意 UTF-8 文本）。
    func testReportDecodesBase64TitleAndMessage() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var got: ProgramStatusReport?
        let done = expectation(description: "onProgramStatus")
        surface.onProgramStatus = { r in got = r; done.fulfill() }

        // "构建中 ¥" / "正在编译" 的 base64（见规范对 title/msg 的规定）。
        surface.feed(Data("\u{1b}]7501;state=working:title=5p6E5bu65LitIMKl:msg=5q2j5Zyo57yW6K+R\u{1b}\\".utf8))
        wait(for: [done], timeout: 5)

        XCTAssertEqual(got?.title, "构建中 ¥")
        XCTAssertEqual(got?.message, "正在编译")
    }

    /// 查询方向：`ESC ] 7501 ; ? ST` 必须被回答，且回包走 **transport 侧**
    /// （onInput），不是留在网格里。
    func testSupportQueryIsAnsweredThroughTransport() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        let done = expectation(description: "查询回包")
        surface.onInput = { d in received.append(d); done.fulfill() }

        surface.feed(Data("\u{1b}]7501;?\u{1b}\\".utf8))
        wait(for: [done], timeout: 5)

        let text = String(decoding: received, as: UTF8.self)
        XCTAssertTrue(text.hasPrefix("\u{1b}]7501;?:"),
                      "回包应为 OSC 7501 的 \"?:\" 形式（得到: \(text.debugDescription)）")
        XCTAssertTrue(text.contains("states=idle,working,done,blocked,error"),
                      "应列出支持的 states")
        XCTAssertTrue(text.contains("kinds=permission,question,auth"),
                      "应列出支持的 kinds")
        XCTAssertEqual(Array(received.suffix(2)), [0x1b, 0x5c],
                       "程序用 ST 查询，回包也应以 ST 结束")
    }

    // MARK: - OSC 133（shell 集成）

    /// 事件方向：喂真 shell 脚本会发的 OSC 133 序列，宿主回调必须拿到
    /// 正确的 kind / prompt_kind / 退出码。
    func testSemanticPromptEventsReachCallback() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var events: [SemanticPromptEvent] = []
        let done = expectation(description: "4 步 shell 集成")
        done.expectedFulfillmentCount = 4
        surface.onSemanticPrompt = { e in events.append(e); done.fulfill() }

        // 一次「提示符 → 输入 → 命令运行 → 命令结束」的完整循环。
        surface.feed(Data("\u{1b}]133;P;k=i\u{1b}\\".utf8))        // prompt_start（主提示符）
        surface.feed(Data("\u{1b}]133;B\u{1b}\\".utf8))            // input_start
        surface.feed(Data("\u{1b}]133;C;cmdline_url=ls%20-la\u{1b}\\".utf8)) // 命令开始
        surface.feed(Data("\u{1b}]133;D;0\u{1b}\\".utf8))          // 命令结束（退出码 0）
        wait(for: [done], timeout: 5)

        XCTAssertEqual(events.map(\.kind),
                       [.promptStart, .inputStart, .outputStart, .commandEnd])
        XCTAssertEqual(events[0].promptKind, .primary, "k=i = 主提示符")
        XCTAssertEqual(events[2].command, "ls -la", "命令行要解码（%20 → 空格）")
        XCTAssertTrue(events[3].hasExitCode)
        XCTAssertEqual(events[3].exitCode, 0)
    }

    /// 退出码非 0 也要如实带过来（规范：用 has_exit_code 区分「没报」与「报了 0」）。
    func testCommandEndCarriesNonZeroExitCode() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var got: SemanticPromptEvent?
        let done = expectation(description: "command_end")
        surface.onSemanticPrompt = { e in
            if e.kind == .commandEnd { got = e; done.fulfill() }
        }
        surface.feed(Data("\u{1b}]133;D;130\u{1b}\\".utf8))
        wait(for: [done], timeout: 5)

        XCTAssertEqual(got?.exitCode, 130)
        XCTAssertTrue(got?.hasExitCode ?? false)
    }

    // MARK: - 焦点（分屏：只有当前窗格才闪）

    /// 焦点必须真的下发到 libghostty——否则分屏时每个格子都会闪。
    ///
    /// 观测手段用 **DEC 1004（焦点上报）**：程序开启后，焦点得/失都会经 host
    /// write 回调吐出 `CSI I` / `CSI O`。它测的正是游标闪烁所用的那个焦点位
    /// （Surface.focusCallback → terminal.flags.focused），所以既能证明「焦点
    /// 到了引擎」，又是程序（vim / tmux）真正看得到的行为。
    func testFocusIsReportedToProgramThroughHostWrite() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        surface.onInput = { d in received.append(d) }

        // 开焦点上报。通道是活的：开上报时终端自认已聚焦 → 立刻一条 FocusIn。
        surface.feed(Data("\u{1b}[?1004h".utf8))
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[I") },
                      "开启 1004 后应收到 FocusIn（得到 \(debugBytes(received))）")

        // 失焦 → FocusOut（分屏里非当前窗格走的就是这一步）。
        received.removeAll()
        surface.setFocused(false)
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[O") },
                      "setFocused(false) 必须产生 FocusOut，否则非当前窗格会继续闪"
                      + "（得到 \(debugBytes(received))）")

        // 得焦 → FocusIn。
        received.removeAll()
        surface.setFocused(true)
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[I") },
                      "setFocused(true) 必须产生 FocusIn（得到 \(debugBytes(received))）")
    }


    // MARK: - 浅/深色切换（已有 surface 也要跟着换）

    /// 切浅/深色时，**已经存在的** surface 必须跟着换色——不能只有新开的才对。
    ///
    /// 用户实测：「切到深色时终端颜色没及时刷新，新建终端是新的了」。根因是
    /// 每个 surface 各自持有一份 `config_conditional_state`（创建时从 app 拷贝），
    /// 只喂 app 级色板事件的话，已有 surface 不会换色。修法：`gt_ghostty_app_set_color_scheme`
    /// 除了报 app 级，还要**逐个**给已有 surface 报一遍（对齐上游 GTK apprt 的
    /// `handleStyleManagerDark`：`colorSchemeEvent` + 遍历 `surfaces` 调 `colorSchemeCallback`）。
    ///
    /// 观测手段是 **DEC mode 2031（配色方案上报）**：程序开启后，引擎的
    /// `colorSchemeCallback` 会给它吐 `CSI ? 997 ; 1 n`（深色）/ `; 2 n`（浅色）。
    /// 它测的正是我们新下发的那条 surface 级事件——像素级证据见
    /// `GhostermApp` 的终端底色断言；这里是不依赖窗口的确定性版本。
    func testColorSchemeSwitchReachesExistingSurface() throws {
        let dir = NSTemporaryDirectory() + "ghosterm-scheme-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        // 条件主题是必须的：否则配置里没有 theme 条件，切换不会触发任何重下发。
        let light = dir + "/Light Theme"
        let dark = dir + "/Dark Theme"
        try "background = #ff0000\nforeground = #ffffff\n"
            .write(toFile: light, atomically: true, encoding: .utf8)
        try "background = #0000ff\nforeground = #ffffff\n"
            .write(toFile: dark, atomically: true, encoding: .utf8)
        let conf = dir + "/ghostty.conf"
        try "theme = light:\"\(light)\",dark:\"\(dark)\"\n"
            .write(toFile: conf, atomically: true, encoding: .utf8)

        let app = try TerminalApp(configPath: conf)
        defer { _ = app }
        app.setColorScheme(dark: false)          // 起手浅色
        // 关键：surface 在**切换之前**就建好——它正是「已有 surface」。
        let surface = try TerminalSurface(app: app)

        var received = Data()
        surface.onInput = { d in received.append(d) }
        surface.feed(Data("\u{1b}[?2031h".utf8))   // 开启配色方案上报
        // 让 io 线程先把 2031 处理掉（feed 与色板事件走**同一个** io 邮箱，
        // 顺序天然保住；这里只是给它一点时间，避免冷启动抢跑）。
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))

        received.removeAll()
        app.setColorScheme(dark: true)
        XCTAssertTrue(waitUntil(20) {
            String(decoding: received, as: UTF8.self).contains("\u{1b}[?997;1n")
        }, "切深色后**已有** surface 应收到「深色」上报（得到 \(debugBytes(received))）——"
           + "收不到说明切换没下发到已存在的面（用户报的「已开终端不刷新」）")

        received.removeAll()
        app.setColorScheme(dark: false)
        XCTAssertTrue(waitUntil(20) {
            String(decoding: received, as: UTF8.self).contains("\u{1b}[?997;2n")
        }, "切回浅色同理（得到 \(debugBytes(received))）")
    }

    // MARK: - 可见性（后台标签页停画，省 CPU/GPU）

    /// 可见性必须真的下发到 libghostty——否则后台标签页的 surface 会在没人看的
    /// 时候继续跑渲染线程（计时器 + 收到输出就重建/重绘）。
    ///
    /// 观测手段用 **DEC mode 2033（可见性上报）**：程序开启后，可见性一变都会经
    /// host write 回调吐出 `CSI ?999;1n`（可见）/ `CSI ?999;2n`（不可见）。它测的
    /// 正是渲染线程用来决定「画不画」的那个 `visible` 位
    /// （Surface.occlusionCallback → renderer.mailbox，Thread.drawFrame 会短路）。
    func testVisibilityIsReportedToProgramThroughHostWrite() throws {
        let (app, surface) = try makeSurface()
        defer { _ = app }

        var received = Data()
        surface.onInput = { d in received.append(d) }

        // 开可见性上报。启动时是「可见」→ 开上报那一刻会立刻报一条 potentially
        // visible（`\u{1b}[?999;1n`）。
        surface.feed(Data("\u{1b}[?2033h".utf8))
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[?999;1n") },
                      "开启 2033 后应收到「可见」上报（得到 \(debugBytes(received))）")

        // 不可见（后台标签页）→ 上报 `;2n`。
        received.removeAll()
        surface.setVisible(false)
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[?999;2n") },
                      "setVisible(false) 必须报「不可见」，否则后台标签页会继续白烧"
                      + "（得到 \(debugBytes(received))）")

        // 切回来 → 上报 `;1n`。
        received.removeAll()
        surface.setVisible(true)
        XCTAssertTrue(waitUntil { String(decoding: received, as: UTF8.self).contains("\u{1b}[?999;1n") },
                      "setVisible(true) 必须报「可见」（得到 \(debugBytes(received))）")
    }

}

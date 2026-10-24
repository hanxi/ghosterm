// GhostermAppTests — M1 竖切：证明「主机列表 → transport → surface」真的通。
//
// 断言原则：能读 libghostty 网格的，就读网格（那是终端引擎的真话）；
// 读不了的（SSH 认证/主机密钥），走真 sshd e2e 或显式 SKIP。
import AppKit
import GhostermAppModel
import GhostermCore
import GhostermTerminal
import XCTest

final class HostListTests: XCTestCase {
    func testParsesHostsAndSkipsWildcardBlocks() throws {
        let text = """
        # 注释
        Host web
          HostName 10.0.0.5
          User deploy
          Port 2222

        Host db
          HostName 10.0.0.6

        Host *
          ServerAliveInterval 60
        """
        let model = try HostListModel(contents: text)
        XCTAssertEqual(model.hosts.map(\.pattern), ["web", "db"],
                       "通配块不是可点主机，不该出现在列表里")
        let web = model.hosts[0]
        XCTAssertEqual(web.hostname, "10.0.0.5")
        XCTAssertEqual(web.user, "deploy")
        XCTAssertEqual(web.port, 2222)
        XCTAssertEqual(web.summary, "deploy@10.0.0.5:2222")
    }

    /// HostName 缺省时，OpenSSH 用 Host 模式本身；列表必须一致。
    func testHostnameFallsBackToPattern() throws {
        let model = try HostListModel(contents: "Host bare\n  User me\n")
        XCTAssertEqual(model.hosts.first?.hostname, "bare")
        XCTAssertEqual(model.hosts.first?.port, 22)
    }

    /// 铁律 #4：列表是只读视图，读它不能改文件内容。
    func testReadingDoesNotMutateConfig() throws {
        let text = "Host web\n  HostName 10.0.0.5\n"
        let model = try HostListModel(contents: text)
        _ = try model.reload()
        XCTAssertEqual(model.rawConfig, text, "读取/求值必须不改变配置原文")
    }

    /// 编辑后：除目标行外逐字节不变，且列表跟着更新。
    func testSparseEditThenReload() throws {
        let text = "Host web\n  HostName 10.0.0.5\n  User deploy\n"
        let model = try HostListModel(contents: text)
        try model.edit(alias: "web", [("User", ["ops"])])
        XCTAssertEqual(model.rawConfig, "Host web\n  HostName 10.0.0.5\n  User ops\n")
        XCTAssertEqual(model.hosts.first?.user, "ops")
    }

    func testIdentityFileResolvesWithPolicy() throws {
        let text = "Host web\n  HostName h\n  IdentityFile ~/.ssh/id_ed25519\n"
        let model = try HostListModel(contents: text)
        XCTAssertEqual(model.hosts.first?.identityFiles, ["~/.ssh/id_ed25519"])
    }
}

final class SessionSmokeTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    /// 本地 shell → transport → libghostty 网格。读的是网格文本。
    func testLocalShellReachesGrid() throws {
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let session = try SessionController.localShell(
            surface: surface, executable: "/bin/sh",
            argv: ["-c", "printf GHOSTERM_LOCAL_OK"],
            kind: .direct
        )
        session.start()
        let state = session.waitUntilExit(timeout: 10)

        guard let text = surface.readText() else {
            return XCTFail("网格读不到内容（状态: \(state)）")
        }
        XCTAssertTrue(text.contains("GHOSTERM_LOCAL_OK"),
                      "本地 PTY 输出应进 libghostty 网格（得到: \(text.debugDescription)）")
    }

    /// SSH 真连：需要 GHOSTERM_SSH_*（由 scripts/apple-gate.sh 起 sshd 注入），
    /// 缺省显式 SKIP（不假装通过）。
    func testSSHShellReachesGrid() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let user = env["GHOSTERM_SSH_USER"],
              let keyPath = env["GHOSTERM_SSH_KEY"],
              let portStr = env["GHOSTERM_SSH_PORT"], let port = UInt16(portStr),
              let pem = try? String(contentsOfFile: keyPath, encoding: .utf8) else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过 SSH e2e")
        }

        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)

        let ssh = try SSHSession(SSHOptions(
            host: host, port: port, username: user,
            knownHostsPath: env["GHOSTERM_SSH_KNOWN_HOSTS"]
        ))
        ssh.setHostKeyDecider { _, _, _, _ in .accept }
        try ssh.authenticate(privateKeyPEM: pem)
        try ssh.connect()

        let session = try SessionController.sshShell(session: ssh, surface: surface)
        session.start()

        // 输入方向：surface.onInput → transport.write（键入语义，非粘贴）。
        surface.sendCommandLine("echo SSH_GRID_$((6*7))")
        XCTAssertTrue(waitForGrid(surface, contains: "SSH_GRID_42"),
                      "远端回显应进 libghostty 网格（得到: \(surface.readText() ?? "")）")

        // resize 方向：surface → transport（SSH window-change）→ 远端 stty。
        // 用一条**带唯一标记**的命令同时回读网格与新尺寸，避免与上一条命令的
        // 旧输出混淆。resize 通知是**异步且合并**的（libghostty 侧 25ms 合并
        // \+ SSH window-change 异步到达），所以一次 stty 可能读到旧尺寸；这里
        // **只做一次**尺寸变更，然后反复用带标记的命令去读，直到某次读到与
        // 网格一致的尺寸。
        // 用**温和**的尺寸变化（约 1.5 倍）：libghostty 会按像素尺寸重建网格，
        // 极端放大（如 20 倍）会一次性申请上百万单元格，测试进程直接 OOM。
        let px = surface.size
        let before = surface.gridSize
        surface.setSize(width: px.width * 3 / 2, height: px.height * 3 / 2)
        Thread.sleep(forTimeInterval: 0.5)
        let want = surface.gridSize
        XCTAssertTrue(want != before, "改屏幕尺寸后网格应变化")

        surface.sendCommandLine("echo RSZ_MARK $(stty size)")
        XCTAssertTrue(waitForGrid(surface, contains: "RSZ_MARK \(want.rows) \(want.cols)",
                                  timeout: 8),
                      "resize 应传到远端（期望 RSZ_MARK \(want.rows) \(want.cols)，得到: \(surface.readText() ?? "")）")
        session.stop()
    }

    /// 轮询网格文本直到出现 needle（或超时）。
    private func waitForGrid(_ surface: TerminalSurface, contains needle: String,
                             timeout: TimeInterval = 15) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if (surface.readText() ?? "").contains(needle) { return true }
            Thread.sleep(forTimeInterval: 0.1)
        }
        return false
    }
}
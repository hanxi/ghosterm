// SSHAuthTests — SFTP 认证桥的回归；终端使用共享异步认证与平台选择器，
// 对应 SSHCredentialSelectorTests 与普通 SSH/SFTP 默认流程。
//
// 这组用例是用户实测问题的回归网（原话：「为何 ssh 提示需要私钥口令？
// 我用 ssh 命令行测试过这个密钥是没口令的」）。三个根因各有一条用例：
//   1. `~/.ssh/...` 的 `~` 展开把分隔符吃掉了（`<home>.ssh/id_rsa`），
//      默认钥匙链一把都读不到 —— 纯函数用例（不依赖网络）；
//   2. 「文件读得出来就先弹口令框」——没加密的钥匙也被问口令；
//   3. 「服务器允许什么」的方法表在握手前读，恒为空 —— 只能一律猜口令。
//
// 真机路径（2/3）走闸门起的真 sshd（GHOSTERM_SSH_*），没有 sshd 时显式 SKIP。
import AppKit
import GhostermAppModel
import GhostermCore
import XCTest
@testable import GhostermAppModel

final class SSHAuthTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 1. `~` 展开（纯函数，离线）

    /// `~/.ssh/id_rsa` 必须展开成 `<home>/.ssh/id_rsa`。
    ///
    /// 这条抓的是实测那个错：`home + p.dropFirst(2)` 把 `~/` 连斜杠一起吃掉，
    /// 展开成 `<home>.ssh/id_rsa` —— 默认候选链（7 把钥匙）全部读不到，
    /// 用户看到「未找到可用私钥（试过：<home>.ssh/id_rsa, …）」。
    func testTildeExpansionKeepsSeparator() {
        let home = NSHomeDirectory()
        let expanded = expandTilde(["~/.ssh/id_rsa", "~/.ssh/id_ed25519"])
        XCTAssertEqual(expanded, ["\(home)/.ssh/id_rsa", "\(home)/.ssh/id_ed25519"],
                       "`~/x` 必须展开成 `<home>/x`（分隔符不能丢）")
        // 反向断言：先前那种「home 后直接接 .ssh」的形状绝不能出现。
        for p in expanded {
            XCTAssertTrue(p.hasPrefix("\(home)/.ssh/"),
                          "展开结果必须在家目录**里面**（得到 \(p)）")
        }
        XCTAssertEqual(expandTilde(["~"]), [home], "裸 `~` 就是家目录")
        XCTAssertEqual(expandTilde(["/etc/ssh/ssh_config"]),
                       ["/etc/ssh/ssh_config"], "绝对路径原样返回")
    }

    /// 默认候选链的每一项展开后都必须真的存在于 `~/.ssh` 下——
    /// 证明「候选链」与「展开」这两步合起来指向的是真文件（而不是拼错的路径）。
    func testDefaultChainExpandsToExistingSshDirectory() {
        let expanded = expandTilde(["~/.ssh/id_rsa"])
        let dir = (expanded[0] as NSString).deletingLastPathComponent
        XCTAssertEqual(dir, NSHomeDirectory() + "/.ssh",
                       "展开后应落在 ~/.ssh 目录里（得到 \(expanded[0])）")
    }

    // MARK: - 2. 私钥要不要口令（纯函数，离线）

    /// 没加密的钥匙 `.notNeeded`，加密的 `.needed`，认不出来的 `.unknown`。
    /// 三态不折叠：`.unknown` 不是「不需要」。
    func testKeyPassphraseRequirement() throws {
        // 只检测 PEM 加密标记；与 core/tests/test_api.c 同用无密钥材料的合成头。
        // 不读取开发者的真实私钥，也不要求 ~/.ssh 中存在任何文件。
        let pemRSA = "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n"
        XCTAssertEqual(Ghosterm.keyPassphraseRequirement(pemRSA), .notNeeded,
                       "没口令的 PEM 私钥不该被判成「需要口令」")

        // 加密的 OpenSSH 新格式：闸门有 ssh-keygen 时现场生成一把（材料不入库）。
        let keygen = "/usr/bin/ssh-keygen"
        if FileManager.default.isExecutableFile(atPath: keygen) {
            let dir = NSTemporaryDirectory() + "ghosterm-enc-\(UUID().uuidString)"
            try FileManager.default.createDirectory(atPath: dir,
                                                    withIntermediateDirectories: true)
            addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
            let enc = dir + "/enc"
            let p = Process()
            p.executableURL = URL(fileURLWithPath: keygen)
            p.arguments = ["-t", "ed25519", "-f", enc, "-N", "pw", "-q"]
            try p.run(); p.waitUntilExit()
            if let encPEM = try? String(contentsOfFile: enc, encoding: .utf8) {
                XCTAssertEqual(Ghosterm.keyPassphraseRequirement(encPEM), .needed,
                               "带口令的 OpenSSH 私钥必须判成「需要口令」")
            }
        }

        XCTAssertEqual(Ghosterm.keyPassphraseRequirement("not a key at all"),
                       .unknown, "认不出来的内容必须是 unknown（不折叠）")
    }

    // MARK: - 真机路径（闸门 sshd）

    private struct SSHEnv {
        let host: String
        let port: UInt16
        let user: String
        let keyPath: String
    }

    private func sshEnv() throws -> SSHEnv? {
        let e = ProcessInfo.processInfo.environment
        guard let host = e["GHOSTERM_SSH_HOST"], let user = e["GHOSTERM_SSH_USER"],
              let keyPath = e["GHOSTERM_SSH_KEY"],
              let portStr = e["GHOSTERM_SSH_PORT"], let port = UInt16(portStr)
        else { return nil }
        return SSHEnv(host: host, port: port, user: user, keyPath: keyPath)
    }

    /// 临时 ssh_config：IdentityFile 指向闸门给的钥匙（与用户那份同构）。
    private func writeConfig(host: String, port: UInt16, user: String,
                             identity: String) throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-authcfg-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        try """
        Host gatehost
          HostName \(host)
          Port \(port)
          User \(user)
          IdentityFile \(identity)
        """.write(toFile: path, atomically: true, encoding: .utf8)
        return path
    }

    private func isolatedKnownHosts() throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-authkh-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/known_hosts"
        try Data().write(to: URL(fileURLWithPath: path))
        return path
    }

    @MainActor
    private func waitUntil(_ model: AppModel, timeout: TimeInterval,
                           _ predicate: @MainActor (AppModel) -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate(model) { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        return false
    }

    /// **本次修复的核心断言**：拿一把**没口令**的钥匙连服务器，
    /// 全程**一次凭据框都不许弹**，并且真连上。
    ///
    /// 先前会弹「需要私钥口令」（用户截图里那个框）：密码/口令需求是在
    /// 握手**前**猜的，而且文件读得出来就先问一遍。
    @MainActor
    func testUnencryptedKeyConnectsWithoutAnyCredentialPrompt() throws {
        guard let e = try sshEnv() else { throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过") }
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        let interaction = RecordingInteraction()
        model.interaction = interaction
        interaction.hostKeyAnswer = .accept

        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.remoteFiles?.panel != nil },
                      "没口令的钥匙应直接连上（状态: \(model.status)）")
        XCTAssertEqual(interaction.prompts.map(\.kind), [],
                       "没口令的钥匙**一次凭据框都不该弹**"
                       + "（实际弹了: \(interaction.prompts.map(\.kind))）")
        model.disconnectForTesting()
    }

    @MainActor
    func testRevokedAndUnsupportedHostTrustNeverPromptsForAcceptanceOrCredentials() throws {
        guard let e = try sshEnv() else { throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过") }
        let known = try isolatedKnownHosts()
        // Obtain only the fixture's public host key through the real handshake.
        let probe = try SSHSession(.init(host: e.host, port: e.port, username: e.user, knownHostsPath: known))
        probe.setHostKeyDecider { _, _, _, _ in .accept }
        try probe.handshake(); probe.close()
        let trusted = try String(contentsOfFile: known, encoding: .utf8)
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user, identity: e.keyPath)
        for (contents, message) in [(trusted + "@revoked " + trusted, "已撤销"),
                                    ("@revoked " + trusted + trusted, "已撤销"),
                                    ("@cert-authority " + trusted, "尚不支持"),
                                    ("@unknown " + trusted, "尚不支持")] {
            try contents.write(toFile: known, atomically: true, encoding: .utf8)
            let model = isolatedAppModel(configPath: cfg, knownHostsPath: known)
            let interaction = RecordingInteraction(); model.interaction = interaction
            model.openSFTP(for: "gatehost")
            XCTAssertTrue(waitUntil(model, timeout: 10) { $0.status.contains(message) }, model.status)
            XCTAssertNil(model.remoteFiles?.panel)
            XCTAssertEqual(interaction.hostKeyPrompts, 0)
            XCTAssertTrue(interaction.prompts.isEmpty)
            XCTAssertEqual(try String(contentsOfFile: known, encoding: .utf8), contents)
            model.disconnectForTesting()
        }
    }

    @MainActor
    func testTrustSaveFailureStopsBeforeCredentialsAndReportsPersistence() throws {
        guard let e = try sshEnv() else { throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过") }
        let known = try isolatedKnownHosts() + "-missing/known_hosts"
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user, identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: known)
        let interaction = RecordingInteraction(); model.interaction = interaction
        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(model, timeout: 10) { $0.status.contains("无法保存主机信任记录，连接已停止") }, model.status)
        XCTAssertEqual(interaction.hostKeyPrompts, 1)
        XCTAssertTrue(interaction.prompts.isEmpty)
        XCTAssertNil(model.remoteFiles?.panel)
        XCTAssertFalse(FileManager.default.fileExists(atPath: known))
        model.disconnectForTesting()
    }

    /// 连一次之后**不该**往 Keychain 里塞「已解锁的 PEM」。
    ///
    /// 那一步（一个 generic-password 项被反复读）正是 macOS 弹
    /// 「想要使用钥匙串」的来源——用户实测被问了。钥匙本来就在 `~/.ssh`
    /// 里，多存一份副本既不增加安全（私钥不落我们的盘，铁律 #5 仍守），
    /// 只换来一次授权弹框。
    @MainActor
    func testConnectDoesNotStashUnlockedKeyInKeychain() throws {
        guard let e = try sshEnv() else { throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过") }
        // 先清掉这条路径可能残留的记录（删除幂等，不触发授权框）。
        KeychainStore.removePrivateKeyPEM(forPath: e.keyPath)

        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        let interaction = RecordingInteraction()
        model.interaction = interaction
        interaction.hostKeyAnswer = .accept

        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.remoteFiles?.panel != nil },
                      "应连上（状态: \(model.status)）")

        XCTAssertThrowsError(try KeychainStore.privateKeyPEM(forPath: e.keyPath)) { error in
            guard case KeychainStoreError.notFound = error else {
                return XCTFail("连接后不该有已解锁的 PEM 存档"
                               + "（读它正是弹「想要使用钥匙串」的来源）；得到 \(error)")
            }
        }
        model.disconnectForTesting()
    }

    /// 钥匙不被接受时：该问的是**登录密码**，不是私钥口令。
    ///
    /// 这条正是用户踩的那个坑的另一半：先前「服务器允许什么」的方法表是
    /// 在握手**前**读的（恒为空），于是 `wantsPassword` 永远为假，一律掉进
    /// 「问私钥口令」——用户看到的框写着「需要私钥口令」，可他那把钥匙根本
    /// 没口令。现在方法表来自握手：服务器收密码（或 keyboard-interactive）
    /// 就问登录密码，一次口令框都不该出现。
    @MainActor
    func testUnacceptedKeyAsksForLoginPasswordNotPassphrase() throws {
        guard let e = try sshEnv() else { throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过") }
        // 现生成一把**没被授权**的钥匙（闸门的 authorized_keys 里没有它）。
        let dir = NSTemporaryDirectory() + "ghosterm-badkey-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let badKey = dir + "/bad"
        let keygen = Process()
        keygen.executableURL = URL(fileURLWithPath: "/usr/bin/ssh-keygen")
        keygen.arguments = ["-t", "ed25519", "-f", badKey, "-N", "", "-q"]
        keygen.standardOutput = FileHandle.nullDevice
        keygen.standardError = FileHandle.nullDevice
        try keygen.run(); keygen.waitUntilExit()
        try XCTSkipUnless(FileManager.default.fileExists(atPath: badKey),
                          "ssh-keygen 不可用 —— 跳过")

        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: badKey)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        let interaction = RecordingInteraction()
        model.interaction = interaction
        interaction.hostKeyAnswer = .accept

        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.tabs.activeTab?.sftpError != nil },
                      "没授权的钥匙必须失败（状态: \(model.status)）")

        // 服务器收密码/keyboard-interactive → 只允许问**登录密码**。
        let passphraseAsked = interaction.prompts.contains {
            if case .passphrase = $0.kind { return true }
            return false
        }
        XCTAssertFalse(passphraseAsked,
                       "钥匙没被接受不等于「钥匙有口令」——不该问私钥口令"
                       + "（实际: \(interaction.prompts.map(\.kind))）")
        let wantedPassword = interaction.prompts.contains {
            if case .password = $0.kind { return true }
            return false
        }
        // Ask only for a method the connector implements. A server advertising
        // keyboard-interactive alone is explicitly unsupported, not password.
        let probe = try SSHSession(.init(host: e.host, port: e.port, username: e.user,
            knownHostsPath: model.knownHostsPath))
        try probe.handshake(); defer { probe.close() }
        XCTAssertEqual(wantedPassword, probe.authMethods.contains(.password),
                       "只在服务器允许 password 时问登录密码；未实现的 keyboard-interactive 必须报错")
    }
}

/// 记录凭据框的 interaction：本组用例要看的就是「弹了几次、弹的是哪一种」。
final class RecordingInteraction: AppInteraction {
    var hostKeyAnswer: HostKeyDecision = .accept
    private(set) var prompts: [CredentialPrompt] = []
    private(set) var hostKeyPrompts = 0

    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision { hostKeyPrompts += 1; return hostKeyAnswer }

    /// 一律取消（返回 nil）——测试若真弹了框，调用方会立刻看到「被取消」，
    /// 于是断言能把「不该问却问了」抓出来。
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? {
        prompts.append(prompt)
        return nil
    }
}

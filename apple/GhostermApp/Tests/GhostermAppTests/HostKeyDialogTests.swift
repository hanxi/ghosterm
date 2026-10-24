// HostKeyDialogTests — 三态对话框的**数据面**断言（真 sshd e2e）。
//
// 证明两件事（M2 验收条款）：
//   1. 「未知被折叠成拒绝」的变异会被这些断言抓不住就说明测试没牙；
//   2. AppModel 的对话框桥（工作线程 → 主线程模态 → 结果回传）真的通：
//      用户点「信任并连接」后连接成功；点「拒绝」连接失败。
//
// 对话框本体（NSAlert）不进这些测试——模态 UI 在无头闸门里没法点；
// 这里注入预设答案的 interaction，验证的是桥与决策链路。
import AppKit
import GhostermAppModel
import GhostermCore
import XCTest

final class HostKeyDialogTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private struct SSHEnv {
        let host: String
        let port: UInt16
        let user: String
        let keyPath: String
        let pem: String
    }

    private func sshEnv() throws -> SSHEnv? {
        let e = ProcessInfo.processInfo.environment
        guard let host = e["GHOSTERM_SSH_HOST"],
              let user = e["GHOSTERM_SSH_USER"],
              let keyPath = e["GHOSTERM_SSH_KEY"],
              let portStr = e["GHOSTERM_SSH_PORT"], let port = UInt16(portStr),
              let pem = try? String(contentsOfFile: keyPath, encoding: .utf8)
        else { return nil }
        return SSHEnv(host: host, port: port, user: user, keyPath: keyPath, pem: pem)
    }

    /// 写一份临时 ssh_config（IdentityFile 指向闸门给的钥匙），
    /// 让 AppModel 按真实解析路径连到闸门 sshd。
    private func writeConfig(host: String, port: UInt16, user: String,
                             identity: String) throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-cfg-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        try """
        Host gatehost
          HostName 127.0.0.1
          Port \(port)
          User \(user)
          IdentityFile \(identity)
        """.write(toFile: path, atomically: true, encoding: .utf8)
        return path
    }

    /// 隔离的 known_hosts：每条用例一个空文件。**绝不**用用户真实
    /// ~/.ssh/known_hosts——decider 接受后 core 会 TOFU 落盘，污染真实
    /// 文件是不可接受的副作用（也保证「未知密钥」前提每条用例都成立）。
    private func isolatedKnownHosts() throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-kh-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/known_hosts"
        try Data().write(to: URL(fileURLWithPath: path))
        return path
    }

    /// SFTP 的接受路径仍支持本机 sshd；终端 Linux 成功路径由默认流程探针验证。
    @MainActor
    func testAcceptDialogConnects() throws {
        guard let e = try sshEnv() else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过")
        }
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        let interaction = ScriptedInteraction()
        model.interaction = interaction
        interaction.hostKeyAnswer = .accept
        interaction.hostKeyInspector = { prompt in
            // 指纹必须真的从握手一路带过来（非空、SHA256 形式）。
            XCTAssertFalse(prompt.fingerprint.isEmpty, "对话框必须拿到指纹")
            XCTAssertTrue(prompt.fingerprint.hasPrefix("SHA256:"),
                          "指纹应为 SHA256 形式（得到 \(prompt.fingerprint)）")
        }

        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.remoteFiles?.panel != nil },
                      "接受未知密钥后应连上（状态: \(model.status)）")
        model.disconnectForTesting()
    }

    /// 用户点「拒绝」→ 连接失败，且不留终端面。
    @MainActor
    func testRejectDialogFailsConnection() throws {
        guard let e = try sshEnv() else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过")
        }
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        let interaction = ScriptedInteraction()
        model.interaction = interaction
        interaction.hostKeyAnswer = .reject

        model.connect(to: .host("gatehost"))
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.status.hasPrefix("连接失败") },
                      "拒绝未知密钥后应连接失败（状态: \(model.status)）")
        XCTAssertNil(model.surface, "失败的连接不得留下终端面")
    }

    /// 无 interaction（无 UI 冒烟）→ 未知密钥显式失败，不静默装成功。
    @MainActor
    func testNoInteractionFailsExplicitly() throws {
        guard let e = try sshEnv() else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过")
        }
        let cfg = try writeConfig(host: e.host, port: e.port, user: e.user,
                                  identity: e.keyPath)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: try isolatedKnownHosts())
        model.interaction = nil // 无 UI

        model.connect(to: .host("gatehost"))
        XCTAssertTrue(waitUntil(model, timeout: 20) { $0.status.hasPrefix("连接失败") },
                      "无对话框时未知密钥必须显式失败（状态: \(model.status)）")
    }

    // MARK: - 辅助

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
}

/// 预设答案的 interaction：hostKeyAnswer 决定裁决；
/// hostKeyInspector 在主线程被调时检视 prompt（真指纹等）。
final class ScriptedInteraction: AppInteraction {
    var hostKeyAnswer: HostKeyDecision = .unknown
    var hostKeyInspector: ((HostKeyPrompt) -> Void)?

    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision {
        hostKeyInspector?(prompt)
        return hostKeyAnswer
    }

    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? {
        nil // 本组测试不覆盖口令路径
    }
}

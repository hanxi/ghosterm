// SFTPBrowserTests — 独立 SFTP 标签页的双栏流 e2e（真 sshd）。
//
// 覆盖（S5 验收条款）：
//   1. openSFTP 建立第二条会话（与终端解耦）并完成初次列目录（远程栏）；
//   2. 远程栏导航与条目内容（目录名 / 尺寸）；
//   3. 经远程 backend 走 TransferManager 下载 → 全局队列到 done；
//   4. 失败 job → failed（不折叠成 done）。
import AppKit
@testable import GhostermAppModel
import GhostermCore
import XCTest

final class SFTPBrowserTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func sshEnv() -> [String: String]? {
        let e = ProcessInfo.processInfo.environment
        guard e["GHOSTERM_SSH_HOST"] != nil, e["GHOSTERM_SSH_PORT"] != nil,
              e["GHOSTERM_SSH_USER"] != nil, e["GHOSTERM_SSH_KEY"] != nil else {
            return nil
        }
        return e
    }

    @MainActor
    private func makeModel(e: [String: String]) throws -> (AppModel, String) {
        let dir = workspaceTestTmp + "sftp-browse-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try """
        Host gatehost
          HostName 127.0.0.1
          Port \(e["GHOSTERM_SSH_PORT"]!)
          User \(e["GHOSTERM_SSH_USER"]!)
          IdentityFile \(e["GHOSTERM_SSH_KEY"]!)
        """.write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")
        model.interaction = AcceptAllInteraction()
        return (model, dir)
    }

    @MainActor
    func testDualPaneBrowseAndDownload() throws {
        guard let e = sshEnv() else {
            throw XCTSkip("未设置 GHOSTERM_SSH_* —— 跳过 SFTP e2e")
        }
        let (model, work) = try makeModel(e: e)
        defer { model.disconnectForTesting() }

        // 预置一个远端文件（即 sshd 侧的本地文件——同一文件系统）。
        let payload = "SFTP_BROWSER_OK"
        let remoteFile = work + "/probe-file.txt"
        try payload.write(toFile: remoteFile, atomically: true, encoding: .utf8)

        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(timeout: 20) { model.remoteFiles?.panel != nil },
                      "SFTP 双栏应建立（状态: \(model.status)）")
        guard let browser = model.remoteFiles?.panel else { return XCTFail("缺远端文件窗格") }

        // 初次列目录（cwd 从 "." 开始——远端用户 home）。
        XCTAssertTrue(waitUntil(timeout: 15) { !browser.entries.isEmpty },
                      "初次列目录应有条目（错误: \(browser.errorText ?? "无")）")
        XCTAssertNil(browser.errorText, "列目录不应报错")

        // 导航到测试工作目录（绝对路径——probe 文件在此）。
        browser.navigate(to: work)
        XCTAssertTrue(waitUntil(timeout: 15) {
            browser.entries.contains { $0.name == "probe-file.txt" }
        }, "进入工作目录应看到预置文件（得到 \(browser.entries.map(\.name).prefix(5))）")
        guard let probe = browser.entries.first(where: { $0.name == "probe-file.txt" }),
              let backend = browser.backend as? TransferBackend else { return XCTFail("缺条目/后端") }

        // 经全局传输队列下载 → done。
        let dst = work + "/downloaded.txt"
        let tm = TransferManager(pump: .shared)   // 进程内只有一个事件泵
        tm.enqueueDownload(backend, remote: probe.path, local: dst,
                           name: probe.name, isDirectory: false)
        XCTAssertTrue(waitUntil(timeout: 20) {
            tm.jobs.first?.status == TransferManager.Status.done
        }, "下载 job 应到 done（得到 \(String(describing: tm.jobs.first?.status))）")
        XCTAssertEqual(try String(contentsOfFile: dst, encoding: .utf8), payload,
                       "下载内容一致")

        // 失败 job 三态：不存在文件 → failed（不是 done）。
        let tm2 = TransferManager(pump: .shared)
        tm2.enqueueDownload(backend, remote: work + "/no-such.txt",
                            local: work + "/dl2.txt", name: "no-such.txt",
                            isDirectory: false)
        XCTAssertTrue(waitUntil(timeout: 20) {
            if case .failed = tm2.jobs.first?.status { return true }
            return false
        }, "失败 job 应到 failed（三态不折叠）")

        model.closeSFTP()
        XCTAssertTrue(waitUntil(timeout: 5) { model.remoteFiles == nil })
    }

    @MainActor
    func testUnifiedWorkspaceTransfersDirectoriesAndEmptyRoots() throws {
        guard let env = sshEnv() else { throw XCTSkip("未设置真 sshd 环境，工作区传输 e2e 未验证") }
        let (model, work) = try makeModel(e: env)
        defer { model.disconnectForTesting() }
        let fm = FileManager.default
        let input = work + "/local-input", output = work + "/remote-output", download = work + "/local-output"
        for path in [input + "/tree/empty", input + "/empty-root", output, download] {
            try fm.createDirectory(atPath: path, withIntermediateDirectories: true)
        }
        try Data("WORKSPACE_PAYLOAD".utf8).write(to: URL(fileURLWithPath: input + "/tree/a.txt"))
        try Data("SINGLE_FILE".utf8).write(to: URL(fileURLWithPath: input + "/one.txt"))
        model.openSFTP(for: "gatehost")
        XCTAssertTrue(waitUntil(timeout: 20) { model.remoteFiles?.panel != nil })
        let tab = try XCTUnwrap(model.tabs.activeTab)
        let local = try XCTUnwrap(tab.localFiles), remote = try XCTUnwrap(tab.remoteFiles)
        let localPanel = try XCTUnwrap(local.panel), remotePanel = try XCTUnwrap(remote.panel)
        localPanel.navigate(to: input); remotePanel.navigate(to: output)
        XCTAssertTrue(waitUntil(timeout: 15) { !localPanel.isLoading && !remotePanel.isLoading })
        let manager = TransferManager(pump: .shared)
        manager.applyToAll = .overwrite
        let upload = WorkspaceFileDrag(paneID: local.id, endpointID: local.endpointID, entries: [
            .init(path: input + "/tree", name: "tree", isDirectory: true),
            .init(path: input + "/empty-root", name: "empty-root", isDirectory: true),
            .init(path: input + "/one.txt", name: "one.txt", isDirectory: false),
        ])
        XCTAssertTrue(WorkspaceFileRouter.perform(upload, onto: remote, directory: output, in: tab, manager: manager))
        XCTAssertTrue(waitUntil(timeout: 30) { !manager.hasActive })
        XCTAssertTrue(manager.jobs.allSatisfy { $0.status == .done }, "目录和空目录都必须有真实完成结果：\(manager.jobs.map { ($0.name, $0.status) })")
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: output + "/tree/a.txt")), Data("WORKSPACE_PAYLOAD".utf8))
        XCTAssertTrue(fm.fileExists(atPath: output + "/tree/empty"))
        XCTAssertTrue(fm.fileExists(atPath: output + "/empty-root"))
        localPanel.navigate(to: download)
        XCTAssertTrue(waitUntil(timeout: 10) { !localPanel.isLoading })
        let fetch = WorkspaceFileDrag(paneID: remote.id, endpointID: remote.endpointID, entries: [
            .init(path: output + "/tree", name: "tree", isDirectory: true),
            .init(path: output + "/empty-root", name: "empty-root", isDirectory: true),
        ])
        XCTAssertTrue(WorkspaceFileRouter.perform(fetch, onto: local, directory: download, in: tab, manager: manager))
        tab.removeFilePane(remote.id)
        XCTAssertTrue(waitUntil(timeout: 30) { !manager.hasActive }, "关文件窗格不得取消已入队传输")
        XCTAssertTrue(manager.jobs.allSatisfy { $0.status == .done })
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: download + "/tree/a.txt")), Data("WORKSPACE_PAYLOAD".utf8))
        XCTAssertTrue(fm.fileExists(atPath: download + "/tree/empty"))
        XCTAssertTrue(fm.fileExists(atPath: download + "/empty-root"))
        XCTAssertTrue(waitUntil(timeout: 10) { localPanel.entries.contains { $0.name == "tree" } }, "完成后刷新落点窗格")
    }

    @MainActor
    private func waitUntil(timeout: TimeInterval,
                           _ predicate: @MainActor () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            CoreEventPump.shared.drain()   // 确定性：不等 Timer，直接抽干队列
            if predicate() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return false
    }
}

/// 无条件接受主机密钥（e2e 环境的 known_hosts 是隔离空文件）。
final class AcceptAllInteraction: AppInteraction {
    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision { .accept }
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? { nil }
}

import AppKit
import Foundation
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class DefaultSessionTests: XCTestCase {
    private func until(file: StaticString = #filePath, line: UInt = #line, _ predicate: () -> Bool) async throws {
        // 六个本地会话要各自走完 daemon 创建 + 快照导入 + 登录 shell 就绪；
        // 每个 shell 都会读 .zshrc，六路并发在高负载机器上 20 秒仍会假红。
        let deadline = Date().addingTimeInterval(30)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "default session flow did not settle", file: file, line: line)
    }
    private func actualPID(_ surface: TerminalSurface?) -> Int32? {
        for text in surface?.readText()?.components(separatedBy: .newlines) ?? [] {
            let line = text.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("LOCAL-PID:"), let pid = Int32(line.dropFirst(10)) { return pid }
        }
        return nil
    }
    /// 存储上的记录。**只能 async 问**：`SessionRecordStore.checkOwner` 要求
    /// 既离开主线程、又落在它自己的 owner 队列上，`queue.sync` 会在主线程上
    /// 就地执行而踩断言。nil = 读失败（按「没清掉」处理，不许放行）。
    nonisolated private static func durableRecords(_ directory: URL) async -> [SessionRecord]? {
        await withCheckedContinuation { (done: CheckedContinuation<[SessionRecord]?, Never>) in
            let owner = DispatchQueue(label: "test.durable-records")
            owner.async { done.resume(returning: try? SessionRecordStore(directory: directory, owner: owner).load()) }
        }
    }
    nonisolated private static func untilDurableGone(_ directory: URL, _ id: UUID) async throws {
        try await untilDurable(directory, timeout: 30) { rows in !rows.contains(where: { $0.id == id }) }
    }
    nonisolated private static func untilDurableEmpty(_ directory: URL) async throws {
        try await untilDurable(directory, timeout: 30) { $0.isEmpty }
    }
    /// 轮询存储直到 `settled` 成立；读失败（nil）一律算「还没清干净」。
    nonisolated private static func untilDurable(_ directory: URL, timeout: TimeInterval,
                                                 _ settled: @escaping ([SessionRecord]) -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        var done = false
        while !done, Date() < deadline {
            if let rows = await durableRecords(directory) { done = settled(rows) }
            else { try? await Task.sleep(nanoseconds: 20_000_000) }
        }
        XCTAssertTrue(done, "durable session records did not settle")
    }
    func testMoreThanFourDefaultLocalSessionsHaveIndependentSourcesAndExplicitStop() async throws {
        _ = NSApplication.shared
        let directory = sessionTestDirectory()
        let model = isolatedAppModel(sessionDirectory: directory); defer { model.disconnectForTesting() }
        for _ in 0..<6 { model.connect(to: .localShell) }
        let panes = model.tabs.tabs.flatMap(\.panes)
        XCTAssertEqual(panes.count, 6)
        try await until { panes.allSatisfy { $0.status == .ready } }
        guard panes.allSatisfy({ $0.status == .ready }) else { return XCTFail("six default sessions: \(panes.map(\.status))") }
        for pane in panes { pane.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r") }
        try await until { panes.allSatisfy { actualPID($0.surface) != nil && ($0.sessionRecord?.session ?? 0) > 0 } }
        XCTAssertEqual(Set(panes.compactMap { actualPID($0.surface) }).count, 6)
        XCTAssertEqual(Set(panes.compactMap { $0.sessionRecord?.session }).count, 6)
        XCTAssertTrue(panes.allSatisfy { $0.sessionRecord?.kind == .local })
        let durableBeforeStop = await Self.durableRecords(directory)
        XCTAssertEqual(durableBeforeStop?.count ?? 0, 6, "终止前六条记录都得在存储上")
        XCTAssertTrue(model.shouldTerminate(), "quit retains daemon Sources even while terminal panes exist")
        for tab in model.tabs.tabs { model.tabs.close(tab) }
        // 终止之后这些记录就打不开了（daemon 在最后一个客户端脱离时已回收
        // Source）：列表与存储都不许留着它们。
        try await until { model.savedSessions.isEmpty }
        try await Self.untilDurableEmpty(directory)
        XCTAssertNil(model.sessionError)
    }
    func testDefaultLocalDetachAndStartupRestoreKeepProcessAndDirectory() async throws {
        _ = NSApplication.shared
        let directory = sessionTestDirectory()
        var first: AppModel? = isolatedAppModel(sessionDirectory: directory)
        first!.connect(to: .localShell)
        guard let pane = first!.tabs.activeTab?.activePane else { return XCTFail("missing pane") }
        try await until { pane.status == .ready && pane.currentDirectory != nil }
        pane.surface?.sendCommittedText("cd /usr; printf 'LOCAL-PID:%s\\n' \"$$\"\r")
        try await until { actualPID(pane.surface) != nil && pane.currentDirectory == "/usr" && (pane.sessionRecord?.session ?? 0) > 0 }
        guard let pid = actualPID(pane.surface), let saved = pane.sessionRecord, let tab = first!.tabs.activeTab else { return XCTFail("original identity missing") }
        let old = pane.surface
        first!.detachKeepingSession(tab)
        try await until { first!.tabs.tabs.isEmpty && first!.savedSessions.contains { $0.id == saved.id } }
        first = nil
        let restored = isolatedAppModel(sessionDirectory: directory); defer { restored.disconnectForTesting() }
        restored.startInitialSessions(); restored.startInitialSessions()
        try await until { restored.tabs.tabs.count == 1 && restored.activePane?.status == .ready && restored.activePane?.currentDirectory == "/usr" }
        XCTAssertEqual(restored.activePane?.sessionRecord?.session, saved.session)
        XCTAssertEqual(restored.activePane?.sessionRecord?.instance, saved.instance)
        XCTAssertEqual(actualPID(restored.surface), pid)
        XCTAssertTrue(restored.surface !== old)
        old?.sendCommittedText("echo STALE-MUST-NOT-EXECUTE\r")
        restored.surface?.sendCommittedText("printf 'RESTORED-OK\\n'\r")
        try await until { restored.surface?.readText()?.components(separatedBy: .newlines).contains { $0.trimmingCharacters(in: .whitespaces) == "RESTORED-OK" } == true }
        XCTAssertFalse(restored.surface?.readText()?.contains("STALE-MUST-NOT-EXECUTE") == true)
        restored.disconnectForTesting()
        // 终止回执落地后这条记录不再列在侧栏、也不再留在存储上。
        try await until { !restored.savedSessions.contains { $0.id == saved.id } }
        try await Self.untilDurableGone(directory, saved.id)
        print("PASS default local entry: durable Source, explicit detach, startup original PID/cwd restore, new Surface and observed stop")
    }
}

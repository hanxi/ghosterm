import AppKit
import Darwin
import Foundation
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class NativeSessionCoordinatorTests: XCTestCase {
    private func until(_ app: TerminalApp, file: StaticString = #filePath, line: UInt = #line, _ predicate: () -> Bool) async throws {
        let end = Date().addingTimeInterval(8)
        while !predicate(), Date() < end { app.tick(); try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "product session coordinator did not settle", file: file, line: line)
    }
    func testCancelBeforeCatalogCompletionCannotPublishFailureOrCreateIntent() async throws {
        let entered = expectation(description: "catalog loader entered"), release = DispatchSemaphore(value: 0)
        let owner = NativeSessionCoordinator(loadCatalog: {
            entered.fulfill(); _ = release.wait(timeout: .now() + 3)
            throw SessionRuntimeCatalog.Failure.unavailable
        }, credentials: { _ in fatalError("cancelled local preparation cannot authenticate") }, changed: { _ in }, failed: { _ in XCTFail("cancelled preparation cannot publish errors") })
        let pane = TerminalPane(), app = try TerminalApp.shared()
        owner.local(pane: pane, app: app, shell: "/bin/sh", cwd: "/")
        await fulfillment(of: [entered], timeout: 2)
        owner.cancelPreparation(pane); release.signal()
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(pane.status, .connecting); XCTAssertNil(pane.sessionRecord); XCTAssertNil(pane.surface)
    }
    func testStoppedPaneRevokesPendingCatalog() async throws {
        let entered = expectation(description: "catalog entered"), release = DispatchSemaphore(value: 0)
        let owner = NativeSessionCoordinator(loadCatalog: {
            entered.fulfill(); _ = release.wait(timeout: .now() + 3)
            throw SessionRuntimeCatalog.Failure.unavailable
        }, credentials: { _ in fatalError("stopped preparation cannot authenticate") }, changed: { _ in }, failed: { _ in XCTFail("stopped preparation cannot publish") })
        let pane = TerminalPane()
        owner.local(pane: pane, app: try TerminalApp.shared(), shell: "/bin/sh", cwd: "/")
        await fulfillment(of: [entered], timeout: 2)
        pane.stop(); release.signal()
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertFalse(pane.status.isFailed, "late catalog failure must not change a stopped pane")
        XCTAssertNil(pane.sessionRecord); XCTAssertNil(pane.surface)
    }
    func testDiscoveryRetriesTransientRecordLockOffMain() async throws {
        let directory = sessionTestDirectory(), io = DispatchQueue(label: "test.catalog.lock")
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Swift.Error>) in
            io.async { do { _ = try SessionRecordStore(directory: directory, owner: io).load(); done.resume() } catch { done.resume(throwing: error) } }
        }
        let fd = open(directory.appendingPathComponent("sessions.lock").path, O_RDWR | O_CLOEXEC | O_NOFOLLOW)
        XCTAssertGreaterThanOrEqual(fd, 0); guard fd >= 0 else { return }
        defer { _ = flock(fd, LOCK_UN); Darwin.close(fd) }
        XCTAssertEqual(flock(fd, LOCK_EX | LOCK_NB), 0)
        var completed = false, failures: [String] = []
        let owner = NativeSessionCoordinator(directory: directory, credentials: { _ in fatalError("discovery cannot authenticate") }, changed: { _ in }, failed: { failures.append($0) })
        owner.refresh { result in if case .success = result { completed = true } }
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertFalse(completed); XCTAssertTrue(failures.isEmpty, "short contention is not a user error")
        XCTAssertEqual(flock(fd, LOCK_UN), 0)
        let deadline = Date().addingTimeInterval(3)
        while !completed, failures.isEmpty, Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(completed); XCTAssertTrue(failures.isEmpty)
    }
    func testLocalCoordinatorSurvivesOwnerReplacementAndExplicitCloseObservesActualTermination() async throws {
        guard ProcessInfo.processInfo.environment["GHOSTERM_NATIVE_COORDINATOR"] == "1" else { throw XCTSkip("requires staged application runtime") }
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let catalog = try await Task.detached { try SessionRuntimeCatalog.loadDefault() }.value
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        if let override = ProcessInfo.processInfo.environment["GHOSTERM_COORDINATOR_TEST_ROOT"] { project = URL(fileURLWithPath: override) }
        let root = project.appendingPathComponent("tmp/coordinator-" + String(UUID().uuidString.prefix(8)))
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let runtime = root.appendingPathComponent(String(catalog.contract.prefix(16)))
        let daemon = Process(); daemon.executableURL = catalog.localDirectory.appendingPathComponent("ghosterm-sessiond")
        daemon.arguments = ["serve", "--runtime-dir", runtime.path]; daemon.standardOutput = FileHandle.nullDevice; daemon.standardError = FileHandle.nullDevice
        try daemon.run()
        defer { if daemon.isRunning { daemon.terminate(); daemon.waitUntilExit() }; try? FileManager.default.removeItem(at: root) }
        let directory = root.appendingPathComponent("records")
        var rows: [SessionRecord] = [], failures: [String] = []
        func coordinator() -> NativeSessionCoordinator {
            NativeSessionCoordinator(directory: directory, runtimeRoot: root, loadCatalog: { catalog },
                credentials: { _ in fatalError("local flow must never authenticate") }, changed: { rows = $0 }, failed: { failures.append($0) })
        }
        var owner: NativeSessionCoordinator? = coordinator()
        let first = TerminalPane(); defer { first.stop() }
        owner!.local(pane: first, app: app, shell: "/bin/sh", cwd: root.path)
        try await until(app) { first.status == .ready || first.status.isFailed }
        guard first.status == .ready, let surface = first.surface else { return XCTFail("local launch: \(first.status)") }
        surface.sendCommittedText("printf 'COORD-PID:%s\\n' \"$$\"\r")
        func actualPID(_ text: String?, prefix: String) -> String? {
            text?.components(separatedBy: .newlines).map { $0.trimmingCharacters(in: .whitespaces) }.first { $0.hasPrefix(prefix) && Int32($0.dropFirst(prefix.count)) != nil }
        }
        try await until(app) { actualPID(surface.readText(), prefix: "COORD-PID:") != nil && (first.sessionRecord?.session ?? 0) > 0 }
        // Use the shell's output, not the echoed input text, as PID evidence.
        let pidLine = actualPID(surface.readText(), prefix: "COORD-PID:")
        XCTAssertNotNil(pidLine)
        surface.sendCommittedText("printf 'COORD-CWD:%s\\n' \"$PWD\"\r")
        try await until(app) { surface.readText()?.components(separatedBy: .newlines).contains { $0.trimmingCharacters(in: .whitespaces) == "COORD-CWD:" + root.path } == true }
        try await until(app) { first.sourceDirectory == root.path && first.childState == .idle }
        let childDirectory = root.appendingPathComponent("目录")
        try FileManager.default.createDirectory(at: childDirectory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let quotedDirectory = "'" + childDirectory.path.replacingOccurrences(of: "'", with: "'\\''") + "'"
        surface.sendCommittedText("cd " + quotedDirectory + "\r")
        try await until(app) { first.sourceDirectory == childDirectory.path }
        surface.sendCommittedText("sleep 1.2; printf 'JOB-DONE\\n'\r")
        try await until(app) { first.childState == .job }
        try await until(app) { first.childState == .idle && surface.readText()?.components(separatedBy: .newlines).contains { $0.trimmingCharacters(in: .whitespaces) == "JOB-DONE" } == true }
        owner!.detached(first)
        owner = nil // new product lifecycle owner must discover durable metadata
        var secondOwner: NativeSessionCoordinator? = coordinator(); secondOwner!.refresh()
        try await until(app) { rows.contains { $0.session > 0 } || !failures.isEmpty }; XCTAssertTrue(failures.isEmpty, "record discovery: \(failures)")
        guard let saved = rows.first(where: { $0.session > 0 }) else { return XCTFail("durable session missing") }
        var incompatible = saved; incompatible.endpoint.contract = String(repeating: "b", count: 64)
        let rejected = TerminalPane()
        secondOwner!.resume(incompatible, pane: rejected, app: app)
        try await until(app) { rejected.status.isFailed }
        XCTAssertNil(rejected.sessionRecord); XCTAssertNil(rejected.surface)
        let second = TerminalPane(); defer { second.stop() }
        secondOwner!.resume(saved, pane: second, app: app)
        try await until(app) { second.status == .ready || second.status.isFailed }
        guard second.status == .ready, let resumed = second.surface else { return XCTFail("local resume: \(second.status)") }
        try await until(app) { second.sourceDirectory == childDirectory.path && second.childState == .idle }
        XCTAssertTrue(resumed !== surface); XCTAssertTrue(resumed.readText()?.contains(pidLine ?? "missing process ID") == true)
        surface.sendCommittedText("echo STALE-MUST-NOT-EXECUTE\r")
        resumed.sendCommittedText("printf 'RESUMED:%s\\n' \"$$\"\r")
        try await until(app) { actualPID(resumed.readText(), prefix: "RESUMED:") != nil }
        XCTAssertFalse(resumed.readText()?.contains("STALE-MUST-NOT-EXECUTE") == true)
        weak var stoppingOwner = secondOwner
        secondOwner!.close(second); secondOwner = nil
        // 终止回执落地后这条记录既打不开也留不住：清单与存储都不许再有它。
        try await until(app) { !rows.contains { $0.id == saved.id } || !failures.isEmpty }
        XCTAssertTrue(failures.isEmpty, "management failures: \(failures)")
        XCTAssertFalse(rows.contains { $0.id == saved.id }, "已结束的记录不许再列进「保留的会话」")
        try await Self.untilDurableGone(directory, saved.id)
        try await until(app) { stoppingOwner == nil }
        // 手动「遗忘记录」那条路径仍然只管删：拿一条还活着的记录验。
        let thirdOwner = coordinator()
        let seed = Self.liveRecord()
        try await Self.write(directory) { try $0.upsert(seed) }
        thirdOwner.refresh()
        try await until(app) { rows.contains { $0.id == seed.id } }
        thirdOwner.forget(seed); try await until(app) { rows.isEmpty }
        print("PASS product local coordinator: real shell, owner replacement, original Source/new Surface, stale input rejected, terminated records reclaimed and explicit forget")
    }

    /// 一条还没结束的本地记录（够 `validate()` 的字段就行）。
    nonisolated private static func liveRecord() -> SessionRecord {
        var value = UUID().uuid
        let nonce = withUnsafeBytes(of: &value) { Array($0) }
        return SessionRecord(kind: .local, endpoint: .init(proxy: "/fixture/proxy", daemon: "/fixture/daemon",
            runtime: "/fixture/runtime", contract: String(repeating: "a", count: 64)), nonce: nonce)
    }
    nonisolated private static func write(_ directory: URL, _ body: @escaping (SessionRecordStore) throws -> Void) async throws {
        let owner = DispatchQueue(label: "test.coordinator.records")
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Swift.Error>) in
            owner.async { do { try body(SessionRecordStore(directory: directory, owner: owner)); done.resume() } catch { done.resume(throwing: error) } }
        }
    }
    /// 存储上的记录。**只能 async 问**：`SessionRecordStore.checkOwner` 要求既
    /// 离开主线程、又落在它自己的 owner 队列上，`queue.sync` 会在主线程就地
    /// 执行而踩断言。nil = 读失败（按「没清掉」处理，不许放行）。
    nonisolated private static func durableRows(_ directory: URL) async -> [SessionRecord]? {
        let owner = DispatchQueue(label: "test.coordinator.read")
        return await withCheckedContinuation { (done: CheckedContinuation<[SessionRecord]?, Never>) in
            owner.async { done.resume(returning: try? SessionRecordStore(directory: directory, owner: owner).load()) }
        }
    }
    nonisolated private static func durableHas(_ directory: URL, _ id: UUID) async -> Bool {
        await durableRows(directory)?.contains(where: { $0.id == id }) ?? true
    }
    nonisolated private static func untilDurableGone(_ directory: URL, _ id: UUID) async throws {
        let deadline = Date().addingTimeInterval(8)
        var gone = !(await durableHas(directory, id))
        while !gone, Date() < deadline {
            try await Task.sleep(nanoseconds: 20_000_000); gone = !(await durableHas(directory, id))
        }
        XCTAssertTrue(gone, "ended record must be reclaimed from the store")
    }
}

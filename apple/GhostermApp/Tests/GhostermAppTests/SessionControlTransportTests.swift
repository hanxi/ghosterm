import AppKit
import Foundation
import Darwin
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class SessionControlTransportTests: XCTestCase {
    private func until(_ app: TerminalApp, _ predicate: () -> Bool) async throws {
        let end = Date().addingTimeInterval(12)
        while !predicate(), Date() < end { app.tick(); try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "management Transport did not complete")
    }
    private func snapshot(_ context: SessionRecordContext) async -> Result<SessionRecord, Swift.Error> {
        await withCheckedContinuation { done in context.snapshot { done.resume(returning: $0) } }
    }
    func testMetadataContextRetriesBriefLockContentionBeforeAdmission() async throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let directory = root.appendingPathComponent("tmp/metadata-retry-" + UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let record = SessionRecord(kind: .local, endpoint: .init(proxy: "/fixture/proxy", daemon: "/fixture/daemon", runtime: "/fixture/runtime", contract: String(repeating: "a", count: 64)), nonce: nonce)
        let context = SessionRecordContext(record: record, directory: directory)
        let initial: Result<Void, Swift.Error> = await withCheckedContinuation { done in context.persistIntent { done.resume(returning: $0) } }
        guard case .success = initial else { return XCTFail("initial private record failed") }
        let fd = open(directory.appendingPathComponent("sessions.lock").path, O_RDWR | O_CLOEXEC | O_NOFOLLOW)
        guard fd >= 0 else { return XCTFail("missing record lock") }
        defer { close(fd) }
        XCTAssertEqual(flock(fd, LOCK_EX | LOCK_NB), 0)
        var saved: Result<Void, Swift.Error>?
        context.persistIntent { result in DispatchQueue.main.async { saved = result } }
        try await Task.sleep(nanoseconds: 100_000_000)
        XCTAssertNil(saved, "brief writer contention must wait on metadata owner rather than fail CREATE")
        XCTAssertEqual(flock(fd, LOCK_UN), 0)
        let end = Date().addingTimeInterval(5)
        while saved == nil, Date() < end { try await Task.sleep(nanoseconds: 10_000_000) }
        guard case .success? = saved else { return XCTFail("bounded metadata retry failed after lock release") }
        guard case .success(let restored) = await snapshot(context) else { return XCTFail("retried record is unreadable") }
        XCTAssertEqual(restored, record)
    }
    func testRealKnownAndPendingCreationCanBeInspectedAndExplicitlyTerminated() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let runtime = env["GHOSTERM_REPLICA_RUNTIME"], let helpers = env["GHOSTERM_REPLICA_HELPERS"],
              let hex = env["GHOSTERM_REPLICA_CONTRACT"] else { throw XCTSkip("run native probe --control") }
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n)
            return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        for pending in [false, true] {
            let directory = URL(fileURLWithPath: runtime).appendingPathComponent("control-records-" + UUID().uuidString)
            let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
            let intent = SessionRecord(kind: .local, endpoint: .init(proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, contract: hex), nonce: nonce)
            let config = RecoverableSession.Options(kind: .local, proxy: intent.endpoint.proxy, daemon: intent.endpoint.daemon, runtime: runtime, compatibility: hash, format: 0x47545643,
                create: .init(nonce: nonce, executable: "/bin/sh", cwd: "/", arguments: ["-c", "printf 'CONTROL-PID:%s\\n' \"$$\"; IFS= read -r never"], size: .init(cols: 80, rows: 24), direct: true))
            let pane = TerminalPane()
            defer { pane.stop() }
            try pane.attachRecoverable(app: app, record: intent, directory: directory, options: config)
            try await until(app) { pane.status == .ready && (pane.sessionRecord?.session ?? 0) > 0 || pane.status.isFailed }
            guard !pane.status.isFailed, let identity = pane.sessionRecord, identity.session > 0 else { return XCTFail("source creation failed") }
            pane.stop()
            var inspected: Result<SessionControlTransport.Outcome, Swift.Error>?
            let inspect = SessionControlTransport(record: identity, directory: directory, operation: .inspect) { inspected = $0 }
            inspect.start()
            try await until(app) { inspected != nil }
            guard case .success(.observed(let found))? = inspected, let active = found else { return XCTFail("live Source was not inspected: \(String(describing: inspected))") }
            XCTAssertEqual(active.session, identity.session); XCTAssertFalse(active.hasResult); XCTAssertFalse(active.terminating)
            // An asynchronous observer error must not disappear behind a later
            // successful file read and falsely publish successful persistence.
            let poisoned = SessionRecordContext(record: identity, directory: directory)
            var wrong = identity.instance; wrong[0] ^= 255
            poisoned.observe(row: active, instance: wrong)
            if case .success = await snapshot(poisoned) { XCTFail("failed observation was hidden by snapshot") }
            var seed = identity, stopDirectory = directory
            if pending {
                seed.session = 0
                stopDirectory = URL(fileURLWithPath: runtime).appendingPathComponent("pending-records-" + UUID().uuidString)
                let pendingRecord = SessionRecordContext(record: seed, directory: stopDirectory)
                let persisted: Result<Void, Swift.Error> = await withCheckedContinuation { done in pendingRecord.persistIntent { done.resume(returning: $0) } }
                if case .failure(let error) = persisted { return XCTFail("pending fixture failed: \(error)") }
            }
            var stopped: Result<SessionControlTransport.Outcome, Swift.Error>?
            var changes: [SessionRecord] = []
            let stop = SessionControlTransport(record: seed, directory: stopDirectory, operation: .terminate, recordChanged: { changes.append($0) }) { stopped = $0 }
            stop.start()
            try await until(app) { stopped != nil }
            guard case .success(.termination(let receipt, let observed))? = stopped, let result = observed else { return XCTFail("explicit stop did not observe result: \(String(describing: stopped))") }
            XCTAssertEqual(receipt.state, .known); XCTAssertEqual(receipt.status, 0); XCTAssertEqual(receipt.session, identity.session)
            XCTAssertTrue(result.hasResult); XCTAssertTrue(result.terminating)
            XCTAssertTrue(changes.contains { $0.disposition == .stopUnknown }, "stop uncertainty must be durably published before the side effect")
            let unknown = changes.firstIndex { $0.disposition == .stopUnknown }
            let ended = changes.firstIndex { $0.resultKnown }
            if let unknown, let ended { XCTAssertLessThan(unknown, ended) } else { XCTFail("missing ordered durable stop facts") }
            let finalContext = SessionRecordContext(record: seed, directory: stopDirectory)
            guard case .success(let saved) = await snapshot(finalContext) else { return XCTFail("termination record was not durably saved") }
            XCTAssertEqual(saved.session, identity.session); XCTAssertEqual(saved.instance, identity.instance)
            XCTAssertTrue(saved.resultKnown); XCTAssertEqual(saved.disposition, .ended)
            if result.hasExitCode { XCTAssertEqual(saved.exitCode, result.exitCode) } else { XCTAssertNil(saved.exitCode, "unknown exit must not turn into 0") }
            var repeated: Result<SessionControlTransport.Outcome, Swift.Error>?
            let again = SessionControlTransport(record: saved, directory: stopDirectory, operation: .terminate) { repeated = $0 }
            again.start()
            try await until(app) { repeated != nil }
            guard case .success(.observed(let final))? = repeated else { return XCTFail("ended Source must not receive another TERMINATE") }
            XCTAssertEqual(final?.hasResult, true)
        }
        let naturalDirectory = URL(fileURLWithPath: runtime).appendingPathComponent("natural-records")
        let naturalNonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let naturalRecord = SessionRecord(kind: .local, endpoint: .init(proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, contract: hex), nonce: naturalNonce)
        let naturalOptions = RecoverableSession.Options(kind: .local, proxy: naturalRecord.endpoint.proxy, daemon: naturalRecord.endpoint.daemon, runtime: runtime, compatibility: hash, format: 0x47545643,
            create: .init(nonce: naturalNonce, executable: "/bin/sh", cwd: "/", arguments: ["-c", "printf 'NATURAL-EXIT\\n'; exit 7"], size: .init(cols: 80, rows: 24), direct: true))
        let naturalPane = TerminalPane()
        defer { naturalPane.stop() }
        try naturalPane.attachRecoverable(app: app, record: naturalRecord, directory: naturalDirectory, options: naturalOptions)
        try await until(app) { naturalPane.sessionRecord?.disposition == .ended || naturalPane.status.isFailed }
        guard let endedRecord = naturalPane.sessionRecord, endedRecord.disposition == .ended else { return XCTFail("natural exit result missing") }
        if let code = endedRecord.exitCode { XCTAssertEqual(code, 7) }
        naturalPane.stop()
        var naturalResult: Result<SessionControlTransport.Outcome, Swift.Error>?
        let naturalStop = SessionControlTransport(record: endedRecord, directory: naturalDirectory, operation: .terminate) { naturalResult = $0 }
        naturalStop.start()
        try await until(app) { naturalResult != nil }
        guard case .success(.observed(let row))? = naturalResult else { return XCTFail("natural exit must be observed without submitting TERMINATE") }
        XCTAssertEqual(row?.hasResult, true); XCTAssertEqual(row?.terminating, false)
        if row?.hasExitCode == true { XCTAssertEqual(row?.exitCode, 7) }
        else { XCTAssertEqual(row?.exitCode, 0); XCTAssertEqual(row?.terminationStatus, 2) }
        if testRun?.failureCount == 0 { print("PASS native control Transport: engine-free inspection, pending nonce lookup, durable explicit stop, observed result and no repeated termination") }
    }
}

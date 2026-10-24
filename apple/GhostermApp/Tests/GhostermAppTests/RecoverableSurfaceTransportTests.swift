import AppKit
import Foundation
import Darwin
import XCTest
import GhostermCore
import GhostermTerminal
@testable import GhostermAppModel

@MainActor final class RecoverableSurfaceTransportTests: XCTestCase {
    private func until(_ app: TerminalApp, timeout: TimeInterval = 12, _ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while !predicate(), Date() < deadline { app.tick(); try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "native transport did not reach the expected observable state")
    }
    private func relay(_ command: String, path: String) async throws {
        try await withCheckedThrowingContinuation { (completion: CheckedContinuation<Void, Error>) in
            DispatchQueue.global().async {
                let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
                guard fd >= 0 else { completion.resume(throwing: NSError(domain: "relay socket", code: Int(errno))); return }
                defer { Darwin.close(fd) }
                var timeout = timeval(tv_sec: 2, tv_usec: 0)
                _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout.size(ofValue: timeout)))
                _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout.size(ofValue: timeout)))
                var address = sockaddr_un(); address.sun_family = sa_family_t(AF_UNIX)
                address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
                let bytes = Array(path.utf8) + [0]
                guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else { completion.resume(throwing: NSError(domain: "relay path", code: 1)); return }
                withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes + Array(repeating: 0, count: $0.count - bytes.count)) }
                let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) } }
                guard connected == 0 else { completion.resume(throwing: NSError(domain: "relay connect", code: Int(errno))); return }
                let sent = command.withCString { Darwin.write(fd, $0, strlen($0)) }
                var reply = [UInt8](repeating: 0, count: 2)
                let received = reply.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
                guard sent == command.utf8.count, received == 2, reply == Array("OK".utf8) else { completion.resume(throwing: NSError(domain: "relay acknowledgement", code: Int(errno))); return }
                completion.resume()
            }
        }
    }
    func testDurableAdmissionPrecedesRealCreateAndFailureNeverFlushes() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let runtime = env["GHOSTERM_REPLICA_RUNTIME"], let helpers = env["GHOSTERM_REPLICA_HELPERS"],
              let hex = env["GHOSTERM_REPLICA_CONTRACT"] else { throw XCTSkip("run native probe --admission") }
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n)
            return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        let owner = DispatchQueue(label: "dev.ghosterm.test.admission-catalog")
        let base = RecoverableSession.Options(kind: .local, proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, compatibility: hash, format: 0x47545643)
        func rows() async throws -> [RecoverableSession.Row] {
            try await withCheckedThrowingContinuation { result in owner.async {
                do {
                    var config = base; config.format = 0; config.catalogOnly = true
                    let client = try RecoverableSession(options: config, owner: owner)
                    let deadline = ProcessInfo.processInfo.systemUptime + 5
                    defer {
                        client.cancel()
                        while !client.close(), ProcessInfo.processInfo.systemUptime < deadline { Thread.sleep(forTimeInterval: 0.001) }
                    }
                    while client.step() != .catalog {
                        guard client.info?.state != .failed, ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("catalog deadline") }
                        Thread.sleep(forTimeInterval: 0.001)
                    }
                    result.resume(returning: try client.catalog() ?? [])
                } catch { result.resume(throwing: error) }
            } }
        }
        let before = try await rows().map(\.session)
        let directory = URL(fileURLWithPath: runtime).appendingPathComponent("records")
        var failures: [String] = [], shown: TerminalSurface?
        var held: ((Result<Void, Swift.Error>) -> Void)?
        var admissionInfo: RecoverableSession.Info?
        let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let record = SessionRecord(kind: .local, endpoint: .init(proxy: base.proxy, daemon: base.daemon, runtime: runtime, contract: hex), nonce: nonce)
        let context = SessionRecordContext(record: record, directory: directory)
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in context.persistIntent { done.resume(with: $0) } }
        let transport = RecoverableSurfaceTransport(app: app, callbacks: .init(surface: { shown = $0 }, state: { _ in }, failure: { failures.append($0) }, admission: { info, completion in
            DispatchQueue.main.async { admissionInfo = info; held = completion }
        }, identity: { context.observe($0) }))
        var config = base
        config.create = .init(nonce: nonce, executable: "/bin/sh", cwd: "/", arguments: ["-c", "printf 'DURABLE-PID:%s\\n' \"$$\"; IFS= read -r line"], size: .init(cols: 80, rows: 24), direct: true)
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in transport.owner.async {
            do { try transport.start(options: config); done.resume() } catch { done.resume(throwing: error) }
        } }
        defer { transport.detach() }
        try await until(app) { held != nil || !failures.isEmpty }
        XCTAssertEqual(failures, [])
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertNil(shown)
        let whileHeld = try await rows().map(\.session)
        XCTAssertEqual(whileHeld, before, "CREATE must stay unsent until durable admission completes")
        guard let identity = admissionInfo, let resume = held else { return XCTFail("missing durable admission callback") }
        XCTAssertTrue(identity.hasAdmittedCreate); XCTAssertEqual(identity.session, 0)
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in context.admit(identity) { done.resume(with: $0) } }
        let saved = try await withCheckedThrowingContinuation { (done: CheckedContinuation<SessionRecord, Error>) in context.snapshot { done.resume(with: $0) } }
        XCTAssertTrue(saved.admitted); XCTAssertEqual(saved.instance, identity.instance); XCTAssertEqual(saved.nonce, nonce)
        resume(.success(()))
        try await until(app) { shown?.readText()?.contains("DURABLE-PID:") == true || !failures.isEmpty }
        XCTAssertEqual(failures, [])
        let afterSuccess = try await rows().map(\.session)
        XCTAssertEqual(afterSuccess.count, before.count + 1)
        shown?.sendCommittedText("done\r")
        // A failed write must synchronously stop stepping. Even a late success
        // completion cannot revive the same CREATE or flush it on a timer tick.
        held = nil; admissionInfo = nil
        let rejected = RecoverableSurfaceTransport(app: app, callbacks: .init(surface: { _ in XCTFail("failed admission created a Surface") }, state: { _ in }, failure: { failures.append($0) }, admission: { info, completion in
            DispatchQueue.main.async { admissionInfo = info; held = completion }
        }))
        config.create?.nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let failedConfig = config
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in rejected.owner.async {
            do { try rejected.start(options: failedConfig); done.resume() } catch { done.resume(throwing: error) }
        } }
        defer { rejected.detach() }
        try await until(app) { held != nil }
        guard let reject = held else { return XCTFail("missing rejected admission callback") }
        reject(.failure(SessionRecordStore.Error.io(EIO)))
        try await until(app) { !failures.isEmpty }
        reject(.success(()))
        try await Task.sleep(nanoseconds: 200_000_000)
        let afterFailure = try await rows().map(\.session)
        XCTAssertEqual(afterFailure, afterSuccess, "failed admission and stale completion must never create a process")
        XCTAssertEqual(failures.count, 1)
        held = nil
        let expired = RecoverableSurfaceTransport(app: app, callbacks: .init(surface: { _ in XCTFail("expired admission created a Surface") }, state: { _ in }, failure: { failures.append($0) }, admission: { _, completion in
            DispatchQueue.main.async { held = completion }
        }))
        config.create?.nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let expiredConfig = config
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in expired.owner.async {
            do { try expired.start(options: expiredConfig); done.resume() } catch { done.resume(throwing: error) }
        } }
        defer { expired.detach() }
        try await until(app) { held != nil }
        guard let late = held else { return XCTFail("missing expiring admission callback") }
        try await until(app, timeout: 7) { failures.count == 2 }
        late(.success(()))
        try await Task.sleep(nanoseconds: 150_000_000)
        let afterExpiry = try await rows().map(\.session)
        XCTAssertEqual(afterExpiry, afterSuccess, "expired durable admission must not flush CREATE")
        XCTAssertEqual(failures.count, 2)
        if testRun?.failureCount == 0 { print("PASS durable native admission: held CREATE, atomic record, real PTY, failed write and stale completion never flush") }
    }
    func testRealLocalDetachResumePreservesProcessAndReplacesSurface() async throws {
        guard let runtime = ProcessInfo.processInfo.environment["GHOSTERM_REPLICA_RUNTIME"],
              let helpers = ProcessInfo.processInfo.environment["GHOSTERM_REPLICA_HELPERS"],
              let hex = ProcessInfo.processInfo.environment["GHOSTERM_REPLICA_CONTRACT"] else {
            throw XCTSkip("run scripts/probe-recoverable-transport.py for the real daemon/PTY/native Surface flow")
        }
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let env = ProcessInfo.processInfo.environment
        let remote = env["GHOSTERM_REPLICA_SSH_PORT"].flatMap(UInt16.init)
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n), b = hex.index(a, offsetBy: 2)
            return UInt8(hex[a..<b], radix: 16)
        }
        var first: TerminalSurface?, second: TerminalSurface?
        var firstInfo: RecoverableSession.Info?, secondInfo: RecoverableSession.Info?
        var failures: [String] = []
        var receipts: [RecoverableSession.Receipt] = []
        var presentations: [TerminalSurface] = []
        func authentication(for transport: RecoverableSurfaceTransport) throws -> RecoverableSession.Authentication? {
            guard let port = remote, let key = env["GHOSTERM_REPLICA_SSH_KEY"], let known = env["GHOSTERM_REPLICA_SSH_KNOWN"] else { return nil }
            let pem = try String(contentsOfFile: key, encoding: .utf8)
            let ssh = SSHOptions(host: "127.0.0.1", port: port, username: env["GHOSTERM_REPLICA_SSH_USER"] ?? NSUserName(), knownHostsPath: known)
            let broker = SessionAuthenticationBroker(owner: transport.owner, options: ssh, prompts: .init(
                credential: { _, _, done in done(.success(.privateKey(pem: pem, passphrase: nil))) },
                hostkey: { _, _, fingerprint, done in
                    let accepted = env["GHOSTERM_REPLICA_FINGERPRINT"] == fingerprint
                    done(accepted ? .accept : .reject)
                    if !accepted { failures.append("fixture hostkey fingerprint mismatch") }
                }), endpoint: { [weak transport] body in transport?.withSession(body) }, failure: { failures.append($0) })
            return broker.callbacks
        }
        let original = RecoverableSurfaceTransport(app: app, callbacks: .init(surface: { first = $0; presentations.append($0) }, state: { firstInfo = $0 }, failure: { failures.append($0) }, receipt: { receipts.append($0) }))
        let create = RecoverableSession.Create(nonce: Array(UUID().uuidString.utf8.prefix(16)), executable: "/bin/sh", cwd: "/",
            arguments: ["-c", "stty -icanon -echo min 1 time 0; printf 'NATIVE-PID:%s\\n' \"$$\"; dd bs=1 count=98304 2>/dev/null | cksum; stty icanon echo; printf 'BULK-READY\\n'; IFS= read -r one; printf 'FIRST:%s\\n' \"$one\"; IFS= read -r two; printf 'SECOND:%s\\n' \"$two\""],
            size: .init(cols: 80, rows: 24), direct: true)
        var options = RecoverableSession.Options(kind: remote == nil ? .local : .ssh, proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, compatibility: hash, format: 0x47545643, create: create)
        options.authenticationTimeout = 3000
        let originalAuth = try authentication(for: original)
        try await withCheckedThrowingContinuation { (result: CheckedContinuation<Void, Error>) in original.owner.async {
            do { try original.start(options: options, authentication: originalAuth); result.resume() } catch { result.resume(throwing: error) }
        } }
        defer { original.detach() }
        try await until(app) { first?.readText()?.contains("NATIVE-PID:") == true || !failures.isEmpty }
        XCTAssertEqual(failures, []); guard let first, let identity = firstInfo else { return XCTFail("missing first native surface or identity") }
        let initial = first.readText() ?? ""
        let pidLine = initial.components(separatedBy: .newlines).first { $0.contains("NATIVE-PID:") }?.trimmingCharacters(in: .whitespaces)
        XCTAssertNotNil(pidLine)
        let encodedSequence: UInt64 = await withCheckedContinuation { result in original.owner.async {
            original.withSession { result.resume(returning: $0.info?.sequence ?? UInt64.max) }
        } }
        XCTAssertNotEqual(encodedSequence, UInt64.max)
        first.sendCommittedText(String(repeating: "x", count: 98304))
        try await until(app) { (first.readText()?.contains("592342754 98304") == true && first.readText()?.contains("BULK-READY") == true && receipts.count >= 3) || !failures.isEmpty }
        XCTAssertEqual(failures, [])
        first.sendCommittedText("one\r")
        try await until(app) { (first.readText()?.contains("FIRST:one") == true && !receipts.isEmpty) || !failures.isEmpty }
        XCTAssertEqual(receipts.first?.stamp.generation, identity.generation)
        XCTAssertEqual(receipts.first?.stamp.sequence, encodedSequence)
        if let relayPath = env["GT_TEST_RELAY_CONTROL"] {
            try await relay("reset", path: relayPath)
            try await until(app, timeout: 30) { presentations.count >= 2 || !failures.isEmpty }
            XCTAssertEqual(failures, []); XCTAssertGreaterThan(firstInfo?.generation ?? 0, identity.generation)
            guard let afterReset = presentations.last else { return XCTFail("missing reset recovery surface") }
            XCTAssertFalse(first === afterReset)
            XCTAssertTrue(afterReset.readText()?.contains(pidLine ?? "impossible-pid") == true)
            let resetGeneration = firstInfo?.generation ?? 0
            try await relay("blackhole", path: relayPath)
            try await until(app, timeout: 30) { firstInfo?.state == .recovering || !failures.isEmpty }
            afterReset.sendCommittedText("stale-network\r")
            try await relay("online", path: relayPath)
            try await until(app, timeout: 30) { presentations.count >= 3 || !failures.isEmpty }
            XCTAssertEqual(failures, []); XCTAssertGreaterThan(firstInfo?.generation ?? 0, resetGeneration)
            XCTAssertTrue(presentations.last?.readText()?.contains(pidLine ?? "impossible-pid") == true)
            XCTAssertFalse(presentations.last === afterReset)
            if testRun?.failureCount == 0 { print("PASS native SSH reset and blackhole recovery: same PID, fresh Surface, higher generations") }
        }
        original.detach()
        // A new owner resumes the exact source; the old view remains readonly.
        options.create = nil; options.session = identity.session; options.instance = identity.instance
        let resumed = RecoverableSurfaceTransport(app: app, callbacks: .init(surface: { second = $0 }, state: { secondInfo = $0 }, failure: { failures.append($0) }))
        let resumedAuth = try authentication(for: resumed)
        try await withCheckedThrowingContinuation { (result: CheckedContinuation<Void, Error>) in resumed.owner.async {
            do { try resumed.start(options: options, authentication: resumedAuth); result.resume() } catch { result.resume(throwing: error) }
        } }
        defer { resumed.detach() }
        try await until(app) { second?.readText()?.contains("FIRST:one") == true || !failures.isEmpty }
        XCTAssertEqual(failures, [])
        guard let second, let recovered = secondInfo else { return XCTFail("missing recovered native surface") }
        XCTAssertFalse(first === second); XCTAssertGreaterThan(recovered.generation, identity.generation)
        XCTAssertEqual(recovered.session, identity.session); XCTAssertEqual(recovered.instance, identity.instance)
        XCTAssertTrue(second.readText()?.contains(pidLine ?? "impossible-pid") == true)
        first.sendCommittedText("stale\r")
        second.sendCommittedText("two\r")
        try await until(app) { second.readText()?.contains("SECOND:two") == true || !failures.isEmpty }
        XCTAssertFalse(second.readText()?.contains("SECOND:stale") == true)
        XCTAssertEqual(failures, [])
        if testRun?.failureCount == 0 { print("PASS real native transport: shared coordinator, PTY input, same process, higher generation, replaced Surface and stale input rejection") }
    }
}

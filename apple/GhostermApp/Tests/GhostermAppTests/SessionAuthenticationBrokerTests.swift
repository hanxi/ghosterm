import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor final class SessionAuthenticationBrokerTests: XCTestCase {
    private func onOwner(_ owner: DispatchQueue, _ work: @escaping () -> Void) async {
        await withCheckedContinuation { done in owner.async { work(); done.resume() } }
    }
    private func until(_ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(6)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 5_000_000) }
        XCTAssertTrue(predicate(), "authentication flow did not settle")
    }
    func testCancellationAndFailedHandshakeNeverRequestCredentialsOrReenterCore() async throws {
        let listener = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        XCTAssertGreaterThanOrEqual(listener, 0); defer { Darwin.close(listener) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); address.sin_family = sa_family_t(AF_INET)
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let bound = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(listener, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        XCTAssertEqual(bound, 0); XCTAssertEqual(Darwin.listen(listener, 4), 0)
        XCTAssertGreaterThanOrEqual(fcntl(listener, F_SETFL, O_NONBLOCK), 0)
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        XCTAssertEqual(withUnsafeMutablePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.getsockname(listener, $0, &length) } }, 0)
        let owner = DispatchQueue(label: "dev.ghosterm.test.auth-flow"), port = UInt16(bigEndian: address.sin_port)
        var prompts = 0, endpoints = 0, observedEndpoints = 0, inCallback = false, failures: [String] = []
        let broker = SessionAuthenticationBroker(owner: owner, options: .init(host: "127.0.0.1", port: port, username: "fixture"),
            prompts: .init(credential: { _, _, _ in prompts += 1 }, hostkey: { _, _, _, _ in prompts += 1 }),
            endpoint: { _ in XCTAssertFalse(inCallback); XCTAssertFalse(Thread.isMainThread); endpoints += 1; DispatchQueue.main.async { observedEndpoints += 1 } }, failure: { failures.append($0) })
        let callbacks = broker.callbacks
        await onOwner(owner) { inCallback = true; callbacks.requested(1, 1); callbacks.cancelled(1); inCallback = false }
        try await Task.sleep(nanoseconds: 30_000_000)
        await onOwner(owner) { XCTAssertEqual(endpoints, 0) }; XCTAssertEqual(prompts, 0)
        let leaked = Darwin.accept(listener, nil, nil)
        XCTAssertLessThan(leaked, 0, "a synchronously revoked request must not even start TCP")
        if leaked >= 0 { Darwin.close(leaked) }
        await onOwner(owner) { inCallback = true; callbacks.requested(2, 2); inCallback = false }
        var accepted: Int32 = -1
        try await until { if accepted < 0 { accepted = Darwin.accept(listener, nil, nil) }; return accepted >= 0 }
        try await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(prompts, 0, "unlocking credentials must wait for SSH host verification and method discovery")
        Darwin.close(accepted)
        try await until { observedEndpoints == 1 }
        await onOwner(owner) { XCTAssertEqual(endpoints, 1); callbacks.hostkey(1, "ssh-ed25519", "SHA256:stale") }
        XCTAssertEqual(prompts, 0)
        await onOwner(owner) { callbacks.requested(3, 3) }
        accepted = -1
        try await until { if accepted < 0 { accepted = Darwin.accept(listener, nil, nil) }; return accepted >= 0 }
        await onOwner(owner) { callbacks.hostkey(1, "ssh-ed25519", "SHA256:stale"); callbacks.cancelled(3) }; Darwin.close(accepted)
        try await Task.sleep(nanoseconds: 30_000_000)
        await onOwner(owner) { XCTAssertEqual(endpoints, 1) }; XCTAssertEqual(failures.count, 0, "transient IO belongs to coordinator recovery, not a fatal pane failure"); XCTAssertEqual(prompts, 0)
    }
    func testRealHandshakeScopesUnknownKeysAndLateDuplicateCredentials() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"].flatMap(UInt16.init),
              let user = env["GHOSTERM_SSH_USER"], let path = env["GHOSTERM_SSH_KEY"] else { throw XCTSkip("requires real sshd fixture") }
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("native-auth-flow-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        let pem = try String(contentsOfFile: path, encoding: .utf8)
        let owner = DispatchQueue(label: "dev.ghosterm.test.auth-flow-real")
        var hostCalls = 0, keys: [(UInt64, (Result<RecoverableSession.Credential, Swift.Error>) -> Void)] = []
        var revoked: [UInt64] = [], accepted: [UInt32] = [], failures: [String] = [], discovered: Int32 = 0
        var endpoints = 0, inCallback = false
        let broker = SessionAuthenticationBroker(owner: owner,
            options: .init(host: host, port: port, username: user, knownHostsPath: directory.appendingPathComponent("known_hosts").path),
            prompts: .init(credential: { id, _, done in
                XCTAssertTrue(Thread.isMainThread); XCTAssertNotEqual(discovered & 1, 0); keys.append((id, done))
            }, hostkey: { _, _, _, done in
                XCTAssertTrue(Thread.isMainThread); hostCalls += 1
                let decision: HostKeyDecision = hostCalls == 1 ? .unknown : .accept
                done(decision); done(decision)
            }, cancelled: { revoked.append($0) }, methods: { _, methods, _ in discovered = methods }, accepted: { _, attempt in accepted.append(attempt) }),
            endpoint: { _ in XCTAssertFalse(inCallback); dispatchPrecondition(condition: .onQueue(owner)); endpoints += 1 }, failure: { failures.append($0) })
        let callbacks = broker.callbacks
        await onOwner(owner) { inCallback = true; callbacks.requested(1, 1); inCallback = false }
        try await until { failures.count == 1 }
        guard !failures.isEmpty else { return XCTFail("unknown hostkey did not complete") }
        XCTAssertTrue(failures[0].contains("未确认")); XCTAssertEqual(keys.count, 0, "unknown hostkey must never unlock or send credentials")
        await onOwner(owner) { XCTAssertEqual(endpoints, 1); callbacks.requested(2, 2) }
        try await until { keys.count == 1 }
        guard keys.count >= 1 else { return XCTFail("trusted host never requested credentials") }
        let stale = keys[0]
        await onOwner(owner) { callbacks.cancelled(2) }
        try await until { revoked.contains(stale.0) }
        stale.1(.success(.privateKey(pem: pem, passphrase: nil))); stale.1(.success(.password("duplicate-fixture-value")))
        try await Task.sleep(nanoseconds: 30_000_000)
        await onOwner(owner) { XCTAssertEqual(endpoints, 1); callbacks.requested(3, 3) }
        try await until { keys.count == 2 }
        guard keys.count >= 2 else { return XCTFail("recovery never requested fresh credentials") }
        XCTAssertNotEqual(keys[1].0, stale.0, "prompt IDs must not repeat across recovery requests")
        keys[1].1(.success(.privateKey(pem: pem, passphrase: nil))); keys[1].1(.success(.password("duplicate-fixture-value")))
        try await until { accepted.count == 1 }
        try await Task.sleep(nanoseconds: 30_000_000)
        await onOwner(owner) { XCTAssertEqual(endpoints, 2); callbacks.hostkey(1, "ssh-ed25519", "SHA256:stale") }
        XCTAssertEqual(accepted, [1]); XCTAssertEqual(hostCalls, 2); XCTAssertEqual(failures.count, 1)
        print("PASS native shared authentication: host/method discovery precedes keys, unknown/duplicate/late scope isolation and authenticated endpoint delivery (adoption verified by C/Linux integration)")
    }
}

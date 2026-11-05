import Cghosterm
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionSessionTests: XCTestCase {
    private func worker(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "native Session owner"), queue = DispatchQueue(label: "ghosterm.extension.session.test")
        queue.async { do { try body(queue) } catch { XCTFail("Session worker: \(error)") }; queue.async { done.fulfill() } }
        wait(for: [done], timeout: 40)
    }
    private func config(_ peer: ExtensionSession.PeerKind = .local, sdk: String = "1.0.0") -> ExtensionSession.Config {
        ExtensionSession.Config(pluginID: "org.ghosterm.preview", pluginVersion: "0.1.0", clientDigest: String(repeating: "d", count: 64),
            executorDigest: String(repeating: "e", count: 64), hostVersion: "0.1.0", sdkVersion: sdk,
            required: ["ui.form"], available: ["ui.form", "ui.chat"], peerKind: peer, peerIdentity: "fixture-native-owner")
    }
    private func channels(_ owner: DispatchQueue) throws -> (ExtensionChannel, ExtensionChannel) {
        var fds: [Int32] = [-1, -1]; guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0 else { throw GhostermError.failed("socketpair") }
        return (try ExtensionChannel(readFD: fds[0], writeFD: fds[0], owner: owner), try ExtensionChannel(readFD: fds[1], writeFD: fds[1], owner: owner))
    }
    private func close(_ s: ExtensionSession) throws { for _ in 0..<32 { if try s.closeStep() { return } }; XCTFail("Session remains draining") }
    private func wait(_ h: ExtensionSession, _ e: ExtensionSession, phase: ExtensionSession.Phase) throws {
        for _ in 0..<2000 { _ = try h.step(); _ = try e.step(); if try h.info().phase == phase { return } }
        XCTFail("Session did not reach \(phase)")
    }
    private func pair(_ owner: DispatchQueue) throws -> (ExtensionSession, ExtensionSession) {
        let (a, b) = try channels(owner)
        let h = try ExtensionSession(channel: a, role: .host, config: config()), e = try ExtensionSession(channel: b, role: .executor, config: config())
        XCTAssertThrowsError(try a.info()); XCTAssertThrowsError(try b.info()); a.stop(); b.stop()
        return (h, e)
    }
    func testNativeConsentAndScopedLease() {
        worker { owner in
            let (h, e) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            try self.wait(h, e, phase: .consent); let i = try h.info()
            XCTAssertEqual(i.decision, .unknown); XCTAssertNil(try h.lease()); XCTAssertEqual(i.peerIdentity, "fixture-native-owner")
            XCTAssertEqual(try h.decide(ticket: i.consentToken, decision: .unknown), 0); XCTAssertNil(try h.lease())
            XCTAssertNotEqual(try h.decide(ticket: String(repeating: "0", count: 32), decision: .allow), 0)
            XCTAssertEqual(try h.info().decision, .unknown); XCTAssertEqual(try h.decide(ticket: i.consentToken, decision: .allow), 0)
            XCTAssertNil(try h.lease()); try self.wait(h, e, phase: .active)
            let l = try XCTUnwrap(h.lease()), other = try XCTUnwrap(e.lease())
            XCTAssertEqual(try h.access(l, capability: "ui.form"), .allowed); XCTAssertEqual(try h.access(l, capability: "ui.chat"), .unsupported)
            XCTAssertEqual(try h.access(other, capability: "ui.form"), .denied)
            XCTAssertNotEqual(try h.decide(ticket: i.consentToken, decision: .allow), 0)
            try h.revoke(); XCTAssertEqual(try h.access(l, capability: "ui.form"), .revoked); XCTAssertNil(try h.lease())
            try self.wait(h, e, phase: .ended); XCTAssertEqual(try h.info().end, .revoked); XCTAssertEqual(try e.info().end, .peer)
        }
    }
    func testDenialDoesNotBecomeUnknown() {
        worker { owner in
            let (h, e) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            try self.wait(h, e, phase: .consent); XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .deny), 0)
            try self.wait(h, e, phase: .ended); XCTAssertEqual(try h.info().decision, .deny); XCTAssertEqual(try e.info().end, .denied)
            XCTAssertNil(try h.lease()); h.stop(); XCTAssertEqual(try h.info().end, .denied)
        }
    }
    func testFailedAdoptionKeepsChannelAndRejectsDirtyIO() {
        worker { owner in
            let (a, b) = try self.channels(owner)
            defer { _ = try? a.closeStep(); _ = try? b.closeStep() }
            XCTAssertThrowsError(try ExtensionSession(channel: a, role: .host, config: self.config(.ssh)))
            XCTAssertThrowsError(try ExtensionSession(channel: a, role: .host, config: self.config(sdk: "2.0.0")))
            XCTAssertEqual(try a.info().phase, .ready); XCTAssertEqual(try a.info().sent, 0)
            XCTAssertEqual(try a.write(Data([1])).count, 1)
            XCTAssertThrowsError(try ExtensionSession(channel: a, role: .host, config: self.config()))
            XCTAssertEqual(try a.info().buffered, 1)
        }
    }
    func testCrossInstanceTicketAndCrossThreadStop() {
        worker { owner in
            let (h, e) = try self.pair(owner), (h2, e2) = try self.pair(owner)
            defer { try? self.close(h); try? self.close(e); try? self.close(h2); try? self.close(e2) }
            try self.wait(h, e, phase: .consent); try self.wait(h2, e2, phase: .consent)
            XCTAssertNotEqual(try h2.decide(ticket: h.info().consentToken, decision: .allow), 0)
            let done = DispatchSemaphore(value: 0)
            DispatchQueue.global().async { h.stop(); done.signal() }; XCTAssertEqual(done.wait(timeout: .now() + 2), .success)
            XCTAssertNil(try h.lease()); XCTAssertEqual(try h.info().end, .revoked); XCTAssertEqual(try h2.info().decision, .unknown)
        }
    }
    func testDeinitActuallyReleasesChannelCapacity() {
        worker { owner in
            let (a, b) = try self.channels(owner); defer { _ = try? b.closeStep() }
            var s: ExtensionSession? = try ExtensionSession(channel: a, role: .host, config: self.config())
            XCTAssertNotNil(s); s = nil
            owner.async {
                ExtensionSession.retryPendingClose(on: owner); XCTAssertEqual(ExtensionSession.pendingCloseCount, 0)
                // Reoccupy the complete Core pool: an empty ledger alone would
                // miss a discarded-but-never-destroyed native Session owner.
                var fresh: [ExtensionChannel] = []
                defer { fresh.forEach { _ = try? $0.closeStep() } }
                do { for _ in 0..<8 { let (a, b) = try self.channels(owner); fresh += [a, b] }; XCTAssertEqual(fresh.count, 16) }
                catch { XCTFail("deinit retained native channel capacity: \(error)") }
            }
        }
    }
    private func fixture() throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_EXT_FIXTURE_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_EXT_FIXTURE_TOKEN"] else { throw XCTSkip("explicit memory fixture required") }
        let fd = socket(AF_INET, SOCK_STREAM, 0); guard fd >= 0 else { throw GhostermError.failed("fixture socket") }; defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard result == 0 else { throw GhostermError.failed("fixture connect") }; var timeout = timeval(tv_sec: 5, tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let auth = Array((token + "\n").utf8); guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture token") }
        var data = Data(), chunk = [UInt8](repeating: 0, count: 4096)
        while true { let n = Darwin.read(fd, &chunk, chunk.count); if n == 0 { break }; guard n > 0, data.count + n <= 1048576 else { throw GhostermError.failed("fixture read") }; data.append(contentsOf: chunk.prefix(n)) }
        guard data.count >= 4, data.prefix(4).reduce(0, { ($0 << 8) | Int($1) }) == data.count - 4,
              let value = try JSONSerialization.jsonObject(with: data.dropFirst(4)) as? [String: Any] else { throw GhostermError.failed("fixture JSON") }; return value
    }
    private func network(_ label: String, deny: Bool) throws {
        let fixture = try fixture()
        worker { owner in
            let route = try XCTUnwrap(fixture[label] as? [String: Any]), ports = try XCTUnwrap(route["ports"] as? [Int])
            let dir = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TMPDIR"]!).appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false); defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("known_hosts").path; try (route["known"] as! String).write(toFile: path, atomically: true, encoding: .utf8)
            let hops = ports.enumerated().map { i, port in ExtensionChannel.Hop(options: SSHOptions(host: "route-hop-\(i).invalid", port: UInt16(port), username: fixture["user"] as! String, knownHostsPath: path), address: i == 0 ? SSHAddress(family: 4, bytes: [127, 0, 0, 1] + Array(repeating: 0, count: 12)) : nil, viaHost: i == 0 ? nil : "127.0.0.1") }
            let channel = try ExtensionChannel(hops: hops, bootstrap: fixture["bootstrap"] as! String, owner: owner)
            var session: ExtensionSession?; defer { if let session { try? self.close(session) } else { _ = try? channel.closeStep() } }
            let end = ProcessInfo.processInfo.systemUptime + 25
            while try channel.step(milliseconds: 5) != .ready {
                let i = try channel.info(); guard i.phase == .connecting, ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("SSH setup") }
                if i.challenge != 0 { XCTAssertEqual(try channel.supply(hop: i.hop, challenge: i.challenge, credential: .privateKey(pem: fixture["pem"] as! String, passphrase: nil)), 0) }
            }
            session = try ExtensionSession(channel: channel, role: .host, config: self.config(.ssh)); let s = session!
            XCTAssertThrowsError(try channel.info()); var decided = false, active = false
            while try s.info().phase != .ended {
                _ = try s.step(milliseconds: 5); let i = try s.info(); guard ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("Session deadline") }
                if i.phase == .consent { XCTAssertFalse(decided); XCTAssertNil(try s.lease()); XCTAssertEqual(i.peerKind, .ssh)
                    XCTAssertEqual(try s.decide(ticket: i.consentToken, decision: deny ? .deny : .allow), 0); decided = true }
                if i.phase == .active { let l = try XCTUnwrap(s.lease()); XCTAssertEqual(try s.access(l, capability: "ui.form"), .allowed); active = true; try s.revoke() }
            }
            XCTAssertTrue(decided); XCTAssertEqual(active, !deny); XCTAssertEqual(try s.info().end, deny ? .denied : .revoked); XCTAssertNil(try s.lease())
        }
    }
    func testTwoHopConsent() throws { try network("two", deny: false) }
    func testThreeHopConsent() throws { try network("three", deny: false) }
    func testTwoHopDenial() throws { try network("two", deny: true) }
}

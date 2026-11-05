import Cghosterm
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionRPCTests: XCTestCase {
    private func worker(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "native RPC owner"), q = DispatchQueue(label: "ghosterm.rpc.test")
        q.async { do { try body(q) } catch { XCTFail("RPC owner: \(error)") }; q.async { done.fulfill() } }
        wait(for: [done], timeout: 45)
    }
    private func methods() -> [ExtensionSession.Method] {
        [ExtensionSession.Method(name: "fixture.echo", capability: "ui.form", receiver: .executor, inputShape: .object, outputShape: .object),
         ExtensionSession.Method(name: "fixture.ask", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
         ExtensionSession.Method(name: "fixture.done", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
         ExtensionSession.Method(name: "fixture.denied", capability: "ui.chat", receiver: .executor)]
    }
    private func config(_ peer: ExtensionSession.PeerKind = .local, enabled: Bool = true) -> ExtensionSession.Config {
        ExtensionSession.Config(pluginID: "org.ghosterm.preview", pluginVersion: "0.1.0", clientDigest: String(repeating: "d", count: 64),
            executorDigest: String(repeating: "e", count: 64), hostVersion: "0.1.0", required: ["ui.form"], available: ["ui.form", "ui.chat"],
            peerKind: peer, peerIdentity: "fixture-native-rpc", methods: enabled ? methods() : nil)
    }
    private func close(_ s: ExtensionSession) throws { for _ in 0..<64 { if try s.closeStep() { return } }; XCTFail("RPC owner remains draining") }
    private func pump(_ h: ExtensionSession, _ e: ExtensionSession) throws { _ = try h.step(); _ = try e.step() }
    private func pair(_ owner: DispatchQueue, enabled: Bool = true) throws -> (ExtensionSession, ExtensionSession, ExtensionSession.Lease, ExtensionSession.Lease) {
        var fd: [Int32] = [-1, -1]; guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fd) == 0 else { throw GhostermError.failed("socketpair") }
        let a = try ExtensionChannel(readFD: fd[0], writeFD: fd[0], owner: owner), b = try ExtensionChannel(readFD: fd[1], writeFD: fd[1], owner: owner)
        let h = try ExtensionSession(channel: a, role: .host, config: config(enabled: enabled)), e = try ExtensionSession(channel: b, role: .executor, config: config(enabled: enabled))
        for _ in 0..<2000 { try pump(h, e); if try h.info().phase == .consent { XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .allow), 0); break } }
        for _ in 0..<2000 { try pump(h, e); if try h.info().phase == .active { return (h, e, try XCTUnwrap(h.lease()), try XCTUnwrap(e.lease())) } }
        throw GhostermError.failed("RPC handshake")
    }
    private func take(_ receiver: ExtensionSession, _ lease: ExtensionSession.Lease, _ h: ExtensionSession, _ e: ExtensionSession) throws -> ExtensionSession.RPCRequest {
        for _ in 0..<2000 { try pump(h, e); let (status, q) = try receiver.takeRequest(lease); XCTAssertTrue(status == .ok || status == .wait); if let q { return q } }
        throw GhostermError.failed("RPC take")
    }
    private func reply(_ s: ExtensionSession, _ l: ExtensionSession.Lease, _ q: ExtensionSession.RPCRequest) throws {
        XCTAssertEqual(try s.checkRequest(l, ticket: q.ticket), .ok)
        let out = Data("{\"kind\":\"success\",\"value\":".utf8) + q.input + Data("}".utf8)
        XCTAssertEqual(try s.respond(l, ticket: q.ticket, outcome: out), .ok)
    }
    private func complete(_ sender: ExtensionSession, _ l: ExtensionSession.Lease, _ id: UInt64, _ h: ExtensionSession, _ e: ExtensionSession) throws -> ExtensionSession.RPCResult {
        for _ in 0..<2000 { try pump(h, e); let (status, value) = try sender.requestResult(l, handle: id); XCTAssertEqual(status, .ok); if let value, value.confirmed { return value } }
        throw GhostermError.failed("RPC result")
    }
    func testBidirectionalSnapshotsSurviveForgetAndClose() {
        worker { owner in
            let (h, e, hl, el) = try self.pair(owner); defer { _ = try? h.closeStep(); _ = try? e.closeStep() }
            let expected = Data("{\"text\":\"中文\",\"n\":17}".utf8); var input = Data(expected)
            let a = try h.request(hl, method: "fixture.echo", input: input); XCTAssertEqual(a.status, .ok); let id = try XCTUnwrap(a.handle)
            input.resetBytes(in: 0..<input.count); let q = try self.take(e, el, h, e); XCTAssertEqual(q.input, expected)
            try self.reply(e, el, q); let r = try self.complete(h, hl, id, h, e); XCTAssertEqual(r.state, .succeeded); XCTAssertTrue(r.confirmed)
            XCTAssertEqual(try h.forgetRequest(hl, handle: id), .ok); XCTAssertEqual(try h.requestResult(hl, handle: id).0, .invalid)
            let reverse = try e.request(el, method: "fixture.ask", input: expected), other = try XCTUnwrap(reverse.handle)
            let rq = try self.take(h, hl, h, e); try self.reply(h, hl, rq); XCTAssertEqual(try self.complete(e, el, other, h, e).state, .succeeded)
            try self.close(h); try self.close(e); XCTAssertEqual(q.input, expected); XCTAssertTrue(String(decoding: r.outcome, as: UTF8.self).contains("中文"))
        }
    }
    func testQueuedCancellationAndSlotBackpressure() {
        worker { owner in
            let (h, e, l, _) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            var handles: [UInt64] = []
            for _ in 0..<64 { handles.append(try XCTUnwrap(h.request(l, method: "fixture.echo", input: Data("{}".utf8)).handle)) }
            let extra = try h.request(l, method: "fixture.echo", input: Data("{}".utf8)); XCTAssertEqual(extra.status, .wait); XCTAssertNil(extra.handle)
            for id in handles { XCTAssertEqual(try h.cancelRequest(l, handle: id), .ok); let r = try XCTUnwrap(h.requestResult(l, handle: id).1)
                XCTAssertEqual(r.state, .cancelled); XCTAssertFalse(r.confirmed); XCTAssertTrue(r.cancelRequested); XCTAssertTrue(r.requestID.isEmpty)
                XCTAssertEqual(try h.forgetRequest(l, handle: id), .ok) }
        }
    }
    func testLostPendingRequestRemainsUnknown() {
        worker { owner in
            let (h, e, hl, el) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            let id = try XCTUnwrap(h.request(hl, method: "fixture.echo", input: Data("{}".utf8)).handle); _ = try self.take(e, el, h, e)
            h.stop(); let r = try XCTUnwrap(h.requestResult(hl, handle: id).1); XCTAssertEqual(r.state, .unknown); XCTAssertFalse(r.confirmed)
            XCTAssertFalse(r.requestID.isEmpty); XCTAssertTrue(String(decoding: r.outcome, as: UTF8.self).contains("unknown"))
        }
    }
    func testConfirmedResultSurvivesStop() {
        worker { owner in
            let (h, e, hl, el) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            let id = try XCTUnwrap(h.request(hl, method: "fixture.echo", input: Data("{}".utf8)).handle); let q = try self.take(e, el, h, e)
            try self.reply(e, el, q); let before = try self.complete(h, hl, id, h, e); h.stop(); let after = try XCTUnwrap(h.requestResult(hl, handle: id).1)
            XCTAssertTrue(after.confirmed); XCTAssertEqual(after.state, .succeeded); XCTAssertEqual(after.outcome, before.outcome)
        }
    }
    func testValidationScopeAndDisabledFeature() {
        worker { owner in
            let (h, e, hl, el) = try self.pair(owner); defer { try? self.close(h); try? self.close(e) }
            for bytes in [Data("[]".utf8), Data("{\"x\":1,\"x\":2}".utf8), Data([0xff]), Data([123, 125, 0])] {
                XCTAssertEqual(try h.request(hl, method: "fixture.echo", input: bytes).status, .invalid) }
            XCTAssertEqual(try h.request(hl, method: "fixture.denied", input: Data("{}".utf8)).status, .denied)
            XCTAssertEqual(try h.request(hl, method: "fixture.ask", input: Data("{}".utf8)).status, .unsupported)
            XCTAssertEqual(try h.takeRequest(el).0, .denied); XCTAssertEqual(try h.requestResult(el, handle: 1).0, .denied)
            let (h2, e2, l2, _) = try self.pair(owner, enabled: false); defer { try? self.close(h2); try? self.close(e2) }
            XCTAssertEqual(try h2.request(l2, method: "fixture.echo", input: Data("{}".utf8)).status, .unsupported)
        }
    }
    private func fixture() throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_EXT_FIXTURE_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_EXT_FIXTURE_TOKEN"] else { throw XCTSkip("explicit memory fixture required") }
        let fd = socket(AF_INET, SOCK_STREAM, 0); guard fd >= 0 else { throw GhostermError.failed("fixture socket") }; defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let status = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }; guard status == 0 else { throw GhostermError.failed("fixture connect") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0); _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let auth = Array((token + "\n").utf8); guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture token") }
        var data = Data(), chunk = [UInt8](repeating: 0, count: 4096)
        while true { let n = Darwin.read(fd, &chunk, chunk.count); if n == 0 { break }; guard n > 0, data.count + n <= 1048576 else { throw GhostermError.failed("fixture read") }; data.append(contentsOf: chunk.prefix(n)) }
        guard data.count >= 4, data.prefix(4).reduce(0, { ($0 << 8) | Int($1) }) == data.count - 4 else { throw GhostermError.failed("fixture prefix") }
        return try XCTUnwrap(JSONSerialization.jsonObject(with: data.dropFirst(4)) as? [String: Any])
    }
    private func network(_ label: String) throws {
        let f = try fixture()
        worker { owner in
            let route = try XCTUnwrap(f[label] as? [String: Any]), ports = try XCTUnwrap(route["ports"] as? [Int])
            let dir = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TMPDIR"]!).appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false); defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("known_hosts").path; try (route["known"] as! String).write(toFile: path, atomically: true, encoding: .utf8)
            let hops = ports.enumerated().map { i, port in ExtensionChannel.Hop(options: SSHOptions(host: "route-hop-\(i).invalid", port: UInt16(port), username: f["user"] as! String, knownHostsPath: path), address: i == 0 ? SSHAddress(family: 4, bytes: [127, 0, 0, 1] + Array(repeating: 0, count: 12)) : nil, viaHost: i == 0 ? nil : "127.0.0.1") }
            let c = try ExtensionChannel(hops: hops, bootstrap: f["bootstrap"] as! String, owner: owner); var session: ExtensionSession?
            defer { if let session { try? self.close(session) } else { _ = try? c.closeStep() } }; let end = ProcessInfo.processInfo.systemUptime + 30
            while try c.step(milliseconds: 5) != .ready { let i = try c.info(); guard i.phase == .connecting, ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("SSH admission") }
                if i.challenge != 0 { XCTAssertEqual(try c.supply(hop: i.hop, challenge: i.challenge, credential: .privateKey(pem: f["pem"] as! String, passphrase: nil)), 0) } }
            session = try ExtensionSession(channel: c, role: .host, config: self.config(.ssh)); let s = session!; var completed = 0, handled = 0; var pending: UInt64?, done: ExtensionSession.RPCRequest?; var answered = false
            while try s.info().phase != .ended { _ = try s.step(milliseconds: 5); let i = try s.info(); guard ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("RPC deadline") }
                if i.phase == .consent { XCTAssertEqual(try s.decide(ticket: i.consentToken, decision: .allow), 0) }
                guard i.phase == .active else { continue }; let l = try XCTUnwrap(s.lease())
                if completed < 80 && pending == nil { pending = try XCTUnwrap(s.request(l, method: "fixture.echo", input: Data("{\"text\":\"bridge\"}".utf8)).handle) }
                let (status, q) = try s.takeRequest(l); XCTAssertTrue(status == .ok || status == .wait)
                if let q { if q.method == "fixture.done" { XCTAssertNil(done); done = q } else { XCTAssertEqual(q.method, "fixture.ask"); handled += 1; try self.reply(s, l, q) } }
                if let id = pending, let r = try s.requestResult(l, handle: id).1, r.confirmed { XCTAssertEqual(r.state, .succeeded); XCTAssertEqual(try s.forgetRequest(l, handle: id), .ok); pending = nil; completed += 1 }
                if let done, !answered, completed == 80 && handled == 80 { try self.reply(s, l, done); answered = true }
            }
            XCTAssertEqual(completed, 80); XCTAssertEqual(handled, 80); XCTAssertTrue(answered); XCTAssertEqual(try s.info().end, .lost)
            try s.revoke(); XCTAssertEqual(try s.info().end, .lost)
        }
    }
    func testTwoHopBidirectionalRPC() throws { try network("two") }
    func testThreeHopBidirectionalRPC() throws { try network("three") }
}

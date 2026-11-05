import Cghosterm
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionResourceTests: XCTestCase {
    private typealias Store = ExtensionResourceStore
    private func worker(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "resource owner"), q = DispatchQueue(label: "ghosterm.resource.test")
        q.async { do { try body(q) } catch { XCTFail("resource owner: \(error)") }; q.async { done.fulfill() } }
        wait(for: [done], timeout: 50)
    }
    private func store(_ owner: DispatchQueue) throws -> (Store, URL) {
        let base = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let dir = base.appendingPathComponent("tmp/native-resource-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); guard fd >= 0 else { throw GhostermError.failed("directory fd") }
        defer { Darwin.close(fd) }; return (try Store(directoryFD: fd, owner: owner), dir)
    }
    private func config(_ peer: ExtensionSession.PeerKind = .local, granted: Bool = true) -> ExtensionSession.Config {
        ExtensionSession.Config(pluginID: "org.ghosterm.preview", pluginVersion: "0.1.0", clientDigest: String(repeating: "d", count: 64), executorDigest: String(repeating: "e", count: 64),
            hostVersion: "0.1.0", required: granted ? ["ui.form", "resource.receive"] : ["ui.form"], available: ["ui.form", "ui.chat", "resource.receive"], peerKind: peer, peerIdentity: "native-resource-owner",
            methods: [.init(name: "fixture.echo", capability: "ui.form", receiver: .executor, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.ask", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.done", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.denied", capability: "ui.chat", receiver: .executor)])
    }
    private func close(_ s: ExtensionSession) throws { for _ in 0..<64 { if try s.closeStep() { return } }; XCTFail("Session still draining") }
    private func pump(_ h: ExtensionSession, _ e: ExtensionSession) throws { _ = try h.step(); _ = try e.step() }
    private func pair(_ owner: DispatchQueue, _ hs: Store, _ es: Store, granted: Bool = true) throws -> (ExtensionSession, ExtensionSession, ExtensionSession.Lease, ExtensionSession.Lease) {
        var fd: [Int32] = [-1, -1]; guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fd) == 0 else { throw GhostermError.failed("socketpair") }
        let a = try ExtensionChannel(readFD: fd[0], writeFD: fd[0], owner: owner), b = try ExtensionChannel(readFD: fd[1], writeFD: fd[1], owner: owner)
        let h = try ExtensionSession(channel: a, role: .host, config: config(granted: granted), resourceStore: hs)
        let e = try ExtensionSession(channel: b, role: .executor, config: config(granted: granted), resourceStore: es)
        for _ in 0..<2000 { try pump(h, e); if try h.info().phase == .consent { XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .allow), 0); break } }
        for _ in 0..<2000 { try pump(h, e); if try h.info().phase == .active { return (h, e, try XCTUnwrap(h.lease()), try XCTUnwrap(e.lease())) } }
        throw GhostermError.failed("resource handshake")
    }
    private func descriptor(_ size: Int, unknown: Bool = false) -> Store.Descriptor {
        Store.Descriptor(sha256: size == 3 ? "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" : size == 0 ? "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" : "31a1f9dea0169551092d05e8bf4a446228c8c3eb4c9b713c66adcb7fd53c89be", mime: "application/octet-stream", displayName: "../展示/\"snapshot\\.bin", size: unknown ? 0 : UInt64(size), sizeKnown: !unknown)
    }
    private func send(_ s: ExtensionSession, _ l: ExtensionSession.Lease, _ receiver: ExtensionSession, _ rl: ExtensionSession.Lease, _ h: ExtensionSession, _ e: ExtensionSession, size: Int = 3, unknown: Bool = false) throws -> Store.Snapshot {
        let a = try s.sendResource(l, descriptor: descriptor(size, unknown: unknown)); XCTAssertEqual(a.status, .ok); let id = try XCTUnwrap(a.handle)
        var at = 0
        for _ in 0..<50000 {
            if at == size { break }; let n = min(768, size - at)
            var data = size == 3 ? Data("abc".utf8).subdata(in: at..<(at + n)) : Data((at..<(at + n)).map { UInt8($0 % 251) })
            let (status, accepted) = try s.writeResource(l, handle: id, bytes: data); XCTAssertTrue(status == .ok || status == .wait); at += accepted; data.resetBytes(in: 0..<data.count); try pump(h, e)
        }
        XCTAssertEqual(at, size); XCTAssertEqual(try s.finishResource(l, handle: id, sourceComplete: true), .ok)
        var confirmed = false
        for _ in 0..<50000 { try pump(h, e); if let r = try s.resourceResult(l, handle: id).1, r.confirmed { XCTAssertEqual(r.state, .succeeded); XCTAssertEqual(r.accepted, UInt64(size)); confirmed = true; break } }
        XCTAssertTrue(confirmed); XCTAssertEqual(try s.forgetResource(l, handle: id), .ok)
        let (status, snap) = try receiver.takeResource(rl); XCTAssertEqual(status, .ok); return try XCTUnwrap(snap)
    }
    private func verify(_ snap: Store.Snapshot, size: Int = 3) throws {
        let (status, info) = try snap.info(); XCTAssertEqual(status, .ok); XCTAssertTrue(try XCTUnwrap(info).published)
        XCTAssertEqual(info?.received, UInt64(size)); XCTAssertEqual(snap.descriptor.sha256, descriptor(size).sha256); XCTAssertEqual(snap.descriptor.displayName, "../展示/\"snapshot\\.bin")
        var at = 0
        while true { let (st, data) = try snap.read(offset: UInt64(at)); XCTAssertEqual(st, .ok)
            if data.isEmpty { break }; XCTAssertEqual(data, size == 3 ? Data("abc".utf8).subdata(in: at..<(at + data.count)) : Data((at..<(at + data.count)).map { UInt8($0 % 251) })); at += data.count }
        XCTAssertEqual(at, size)
    }
    func testRetainedSnapshotOutlivesSessionAndRemainsCharged() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed) }
            let (h, e, hl, el) = try self.pair(q, hs, es)
            XCTAssertEqual(try hs.close(), .wait); XCTAssertEqual(try hs.usage().sessions, 1)
            let snap = try self.send(h, hl, e, el, h, e); let (status, retained) = try snap.retain(); XCTAssertEqual(status, .ok)
            XCTAssertEqual(try snap.info().1?.references, 2); XCTAssertEqual(try snap.close(), .ok); try self.close(h); try self.close(e)
            XCTAssertEqual(try es.usage().sessions, 0); XCTAssertEqual(try es.close(), .wait); XCTAssertEqual(try es.usage().reservedBytes, 3)
            let copy = try XCTUnwrap(retained); try self.verify(copy); XCTAssertEqual(try copy.close(), .ok)
            XCTAssertEqual(try es.close(), .ok); XCTAssertEqual(try hs.close(), .ok)
        }
    }
    func testLargeUnknownAndReverseEmpty() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed) }
            let (h, e, hl, el) = try self.pair(q, hs, es)
            let snap = try self.send(h, hl, e, el, h, e, size: 262144, unknown: true); try self.verify(snap, size: 262144); XCTAssertEqual(try snap.close(), .ok)
            let empty = try self.send(e, el, h, hl, h, e, size: 0); try self.verify(empty, size: 0); XCTAssertEqual(try empty.close(), .ok)
            try self.close(h); try self.close(e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testQueuedCancellationAndLostAreDifferent() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed) }
            let (h, e, hl, _) = try self.pair(q, hs, es)
            let id = try XCTUnwrap(h.sendResource(hl, descriptor: self.descriptor(3)).handle)
            let extra = try h.sendResource(hl, descriptor: self.descriptor(3)); XCTAssertEqual(extra.status, .wait); XCTAssertNil(extra.handle)
            XCTAssertEqual(try h.cancelResource(hl, handle: id), .ok); XCTAssertEqual(try h.resourceResult(hl, handle: id).1?.state, .cancelled)
            XCTAssertFalse(try XCTUnwrap(h.resourceResult(hl, handle: id).1).confirmed); XCTAssertEqual(try h.forgetResource(hl, handle: id), .ok)
            let pending = try XCTUnwrap(h.sendResource(hl, descriptor: self.descriptor(3)).handle)
            for _ in 0..<100 { try self.pump(h, e) }; h.stop(); _ = try h.step()
            XCTAssertEqual(try h.resourceResult(hl, handle: pending).1?.state, .unknown)
            try self.close(h); try self.close(e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testScopeAndCapabilityDenialDoesNotReserve() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed) }
            let (h, e, hl, el) = try self.pair(q, hs, es, granted: false)
            XCTAssertEqual(try h.sendResource(hl, descriptor: self.descriptor(3)).status, .denied)
            XCTAssertEqual(try h.takeResource(el).0, .denied); XCTAssertEqual(try es.usage().resources, 0)
            try self.close(h); try self.close(e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testDroppedSnapshotAndUnhandshakenSessionReleaseOnOwner() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q)
            let (h, e, hl, el) = try self.pair(q, hs, es)
            var snapshot: Store.Snapshot? = try self.send(h, hl, e, el, h, e)
            XCTAssertNotNil(snapshot); try self.close(h); try self.close(e)
            var fd: [Int32] = [-1, -1]; XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &fd), 0)
            let c = try ExtensionChannel(readFD: fd[0], writeFD: fd[0], owner: q)
            var pending: ExtensionSession? = try ExtensionSession(channel: c, role: .host, config: self.config(), resourceStore: es)
            XCTAssertNotNil(pending); XCTAssertEqual(try es.usage().sessions, 1); XCTAssertEqual(try es.close(), .wait)
            pending = nil; snapshot = nil; Darwin.close(fd[1])
            // Both destructors enqueue onto q; this observation runs after the
            // actual Core Session destroy and final snapshot release.
            q.async {
                do {
                    let usage = try es.usage(); XCTAssertEqual(usage.sessions, 0); XCTAssertEqual(usage.resources, 0); XCTAssertEqual(usage.reservedBytes, 0)
                    XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
                    try FileManager.default.removeItem(at: hd); try FileManager.default.removeItem(at: ed)
                } catch { XCTFail("resource destructor: \(error)") }
            }
        }
    }
    private func reply(_ s: ExtensionSession, _ l: ExtensionSession.Lease, _ q: ExtensionSession.RPCRequest) throws {
        XCTAssertEqual(try s.checkRequest(l, ticket: q.ticket), .ok)
        XCTAssertEqual(try s.respond(l, ticket: q.ticket, outcome: Data("{\"kind\":\"success\",\"value\":".utf8) + q.input + Data("}".utf8)), .ok)
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
            let (resourceStore, resourceDir) = try self.store(owner)
            defer { _ = try? resourceStore.close(); try? FileManager.default.removeItem(at: resourceDir) }
            let route = try XCTUnwrap(f[label] as? [String: Any]), ports = try XCTUnwrap(route["ports"] as? [Int])
            let dir = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TMPDIR"]!).appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false); defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("known_hosts").path; try (route["known"] as! String).write(toFile: path, atomically: true, encoding: .utf8)
            let hops = ports.enumerated().map { i, port in ExtensionChannel.Hop(options: SSHOptions(host: "route-hop-\(i).invalid", port: UInt16(port), username: f["user"] as! String, knownHostsPath: path), address: i == 0 ? SSHAddress(family: 4, bytes: [127, 0, 0, 1] + Array(repeating: 0, count: 12)) : nil, viaHost: i == 0 ? nil : "127.0.0.1") }
            let c = try ExtensionChannel(hops: hops, bootstrap: f["bootstrap"] as! String, owner: owner); var session: ExtensionSession?
            defer { if let session { try? self.close(session) } else { _ = try? c.closeStep() } }; let end = ProcessInfo.processInfo.systemUptime + 30
            while try c.step(milliseconds: 5) != .ready { let i = try c.info(); guard i.phase == .connecting, ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("SSH admission") }
                if i.challenge != 0 { XCTAssertEqual(try c.supply(hop: i.hop, challenge: i.challenge, credential: .privateKey(pem: f["pem"] as! String, passphrase: nil)), 0) } }
            session = try ExtensionSession(channel: c, role: .host, config: self.config(.ssh), resourceStore: resourceStore); let s = session!; var completed = 0, handled = 0; var pending: UInt64?, done: ExtensionSession.RPCRequest?; var answered = false
            var resourceID: UInt64?, written = 0; var resourceFinished = false, resourceReceived = false, resourceReady = false
            while try s.info().phase != .ended { _ = try s.step(milliseconds: 5); let i = try s.info(); guard ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("RPC deadline") }
                if i.phase == .consent { XCTAssertEqual(try s.decide(ticket: i.consentToken, decision: .allow), 0) }
                guard i.phase == .active else { continue }; let l = try XCTUnwrap(s.lease())
                if resourceID == nil { resourceID = try XCTUnwrap(s.sendResource(l, descriptor: self.descriptor(262144)).handle) }
                let resource = resourceID!
                if written < 262144 {
                    let n = min(768, 262144 - written), data = Data((written..<(written + min(768, 262144 - written))).map { UInt8($0 % 251) })
                    XCTAssertEqual(data.count, n); let (st, accepted) = try s.writeResource(l, handle: resource, bytes: data)
                    XCTAssertTrue(st == .ok || st == .wait); written += accepted
                }
                if written == 262144 && !resourceFinished { XCTAssertEqual(try s.finishResource(l, handle: resource, sourceComplete: true), .ok); resourceFinished = true }
                if !resourceReceived { let (st, snap) = try s.takeResource(l); XCTAssertTrue(st == .ok || st == .wait)
                    if let snap { try self.verify(snap, size: 262144); XCTAssertEqual(try snap.close(), .ok); resourceReceived = true } }
                if let r = try s.resourceResult(l, handle: resource).1, r.confirmed { XCTAssertEqual(r.state, .succeeded); resourceReady = resourceReceived }
                if completed < 80 && pending == nil { pending = try XCTUnwrap(s.request(l, method: "fixture.echo", input: Data("{\"text\":\"bridge\"}".utf8)).handle) }
                let (status, q) = try s.takeRequest(l); XCTAssertTrue(status == .ok || status == .wait)
                if let q { if q.method == "fixture.done" { XCTAssertNil(done); done = q } else { XCTAssertEqual(q.method, "fixture.ask"); handled += 1; try self.reply(s, l, q) } }
                if let id = pending, let r = try s.requestResult(l, handle: id).1, r.confirmed { XCTAssertEqual(r.state, .succeeded); XCTAssertEqual(try s.forgetRequest(l, handle: id), .ok); pending = nil; completed += 1 }
                if let done, !answered, completed == 80 && handled == 80 && resourceReady { try self.reply(s, l, done); answered = true }
            }
            XCTAssertEqual(completed, 80); XCTAssertEqual(handled, 80); XCTAssertTrue(answered); XCTAssertTrue(resourceReady); XCTAssertEqual(try s.info().end, .lost)
            try s.revoke(); XCTAssertEqual(try s.info().end, .lost)
        }
    }
    func testTwoHopBidirectionalResourcesAndRPC() throws { try network("two") }
    func testThreeHopBidirectionalResourcesAndRPC() throws { try network("three") }
}

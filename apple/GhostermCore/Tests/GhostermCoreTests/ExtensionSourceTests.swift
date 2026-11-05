import Cghosterm
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionSourceTests: XCTestCase {
    private typealias Store = ExtensionResourceStore
    private typealias Source = ExtensionFileSource
    private final class PairOwner {
        var h: ExtensionSession?
        let e: ExtensionSession, hl: ExtensionSession.Lease, el: ExtensionSession.Lease
        init(_ h: ExtensionSession, _ e: ExtensionSession, _ hl: ExtensionSession.Lease, _ el: ExtensionSession.Lease) { self.h = h; self.e = e; self.hl = hl; self.el = el }
    }
    private func worker(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "scoped source owner"), q = DispatchQueue(label: "ghosterm.source.test")
        q.async { do { try body(q) } catch { XCTFail("source owner: \(error)") }; q.async { q.async { done.fulfill() } } }; wait(for: [done], timeout: 50)
    }
    private func directory() throws -> URL {
        let base = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let dir = base.appendingPathComponent("tmp/native-source-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700]); return dir
    }
    private func store(_ owner: DispatchQueue) throws -> (Store, URL) {
        let dir = try directory(), fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); guard fd >= 0 else { throw GhostermError.failed("root") }; defer { Darwin.close(fd) }
        return (try Store(directoryFD: fd, owner: owner), dir)
    }
    private func config(_ peer: ExtensionSession.PeerKind = .local, fileGrant: Bool = true) -> ExtensionSession.Config {
        ExtensionSession.Config(pluginID: "org.ghosterm.preview", pluginVersion: "0.1.0", clientDigest: String(repeating: "d", count: 64), executorDigest: String(repeating: "e", count: 64), hostVersion: "0.1.0",
            required: fileGrant ? ["ui.form", "resource.receive", "file.readScoped"] : ["ui.form", "resource.receive"], available: ["ui.form", "ui.chat", "resource.receive", "file.readScoped"], peerKind: peer, peerIdentity: "native-scoped-source",
            methods: [.init(name: "fixture.echo", capability: "ui.form", receiver: .executor, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.ask", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.done", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object),
                      .init(name: "fixture.denied", capability: "ui.chat", receiver: .executor)])
    }
    private func pair(_ q: DispatchQueue, _ hs: Store, _ es: Store, fileGrant: Bool = true) throws -> PairOwner {
        var fd: [Int32] = [-1, -1]; guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fd) == 0 else { throw GhostermError.failed("socketpair") }
        let a = try ExtensionChannel(readFD: fd[0], writeFD: fd[0], owner: q), b = try ExtensionChannel(readFD: fd[1], writeFD: fd[1], owner: q)
        let h = try ExtensionSession(channel: a, role: .host, config: config(fileGrant: fileGrant), resourceStore: hs)
        let e = try ExtensionSession(channel: b, role: .executor, config: config(fileGrant: fileGrant), resourceStore: es)
        for _ in 0..<2000 { _ = try h.step(); _ = try e.step(); if try h.info().phase == .consent { XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .allow), 0) }
            if try h.info().phase == .active { return PairOwner(h, e, try XCTUnwrap(h.lease()), try XCTUnwrap(e.lease())) } }; throw GhostermError.failed("source handshake")
    }
    private func close(_ s: ExtensionSession) throws { for _ in 0..<64 { if try s.closeStep() { return } }; throw GhostermError.failed("Session draining") }
    private func pump(_ p: PairOwner) throws { if let h = p.h { _ = try h.step() }; _ = try p.e.step() }
    private func file(_ dir: URL, size: Int = 3) throws {
        let bytes = size == 3 ? Data("abc".utf8) : Data((0..<size).map { UInt8($0 % 251) }); try bytes.write(to: dir.appendingPathComponent("file.bin"))
    }
    private func source(_ s: ExtensionSession, _ lease: ExtensionSession.Lease, _ dir: URL, path: String = "file.bin", limit: UInt64 = 262144) throws -> Source.Admission {
        let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); guard fd >= 0 else { throw GhostermError.failed("source root") }; defer { Darwin.close(fd) }
        return try s.openSource(lease, rootFD: fd, relativePath: path, byteLimit: limit)
    }
    private func ready(_ source: Source) throws { for _ in 0..<100 { let phase = try source.step(); XCTAssertTrue(phase == .hashing || phase == .ready); if phase == .ready { return } }; throw GhostermError.failed("source hash") }
    private func verify(_ snapshot: Store.Snapshot, size: Int) throws {
        XCTAssertEqual(snapshot.descriptor.displayName, "file.bin"); XCTAssertEqual(snapshot.descriptor.size, UInt64(size)); XCTAssertEqual(try snapshot.info().1?.published, true)
        var at = 0
        while true { let (status, bytes) = try snapshot.read(offset: UInt64(at)); XCTAssertEqual(status, .ok); if bytes.isEmpty { break }
            XCTAssertEqual(bytes, size == 3 ? Data("abc".utf8).subdata(in: at..<(at + bytes.count)) : Data((at..<(at + bytes.count)).map { UInt8($0 % 251) })); at += bytes.count }; XCTAssertEqual(at, size)
    }
    func testFileReceiptAndBorrowPinSurviveParentClose() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed); try? FileManager.default.removeItem(at: input) }
            try self.file(input); let p = try self.pair(q, hs, es), h = p.h!; let admission = try self.source(h, p.hl, input); XCTAssertEqual(admission.status, .ok); let source = try XCTUnwrap(admission.source)
            XCTAssertEqual(try source.send(), .wait); try self.ready(source); XCTAssertEqual(try source.info().hashed, 3); XCTAssertEqual(try source.info().descriptor.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
            XCTAssertEqual(try source.send(), .ok); var completed = false
            for _ in 0..<5000 { _ = try source.step(); try self.pump(p); if try source.info().phase == .succeeded { completed = true; break } }; XCTAssertTrue(completed)
            let saved = try source.info(); XCTAssertTrue(saved.hasTransfer && saved.transfer.confirmed); XCTAssertEqual(saved.readBytes, 3); XCTAssertEqual(saved.accepted, 3); XCTAssertEqual(saved.transfer.state, .succeeded)
            let (status, ref) = try p.e.takeResource(p.el); XCTAssertEqual(status, .ok); let snap = try XCTUnwrap(ref); try self.verify(snap, size: 3)
            XCTAssertFalse(try h.closeStep()); XCTAssertEqual(try source.closeStep(), .ok); try self.close(h); try self.close(p.e)
            XCTAssertTrue(saved.transfer.confirmed); try self.verify(snap, size: 3); XCTAssertEqual(try snap.close(), .ok); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testChangeAndCancelBeforeSendDoNotPublish() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed); try? FileManager.default.removeItem(at: input) }
            try self.file(input); let p = try self.pair(q, hs, es), source = try XCTUnwrap(self.source(p.h!, p.hl, input).source); try self.ready(source)
            try Data("changed".utf8).write(to: input.appendingPathComponent("file.bin")); XCTAssertEqual(try source.send(), .changed); XCTAssertEqual(try source.info().error, .changed); XCTAssertFalse(try source.info().hasTransfer); XCTAssertEqual(try source.closeStep(), .ok)
            let cancelled = try XCTUnwrap(self.source(p.h!, p.hl, input).source); try cancelled.cancel(); XCTAssertEqual(try cancelled.info().phase, .stopped); XCTAssertEqual(try cancelled.info().error, .cancelled); XCTAssertFalse(try cancelled.info().hasTransfer); XCTAssertEqual(try cancelled.closeStep(), .ok)
            XCTAssertEqual(try es.usage().resources, 0); try self.close(p.h!); try self.close(p.e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testScopePathsAndMissingFileGrantAreExplicit() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed); try? FileManager.default.removeItem(at: input) }
            try self.file(input); let p = try self.pair(q, hs, es)
            XCTAssertEqual(try self.source(p.h!, p.el, input).status, .denied); XCTAssertEqual(try self.source(p.h!, p.hl, input, path: "../file.bin").status, .denied)
            XCTAssertEqual(try self.source(p.h!, p.hl, input, path: input.appendingPathComponent("file.bin").path).status, .invalid)
            XCTAssertEqual(try self.source(p.h!, p.hl, input, path: "missing").status, .notFound); XCTAssertEqual(try self.source(p.h!, p.hl, input, limit: 2).status, .limit)
            try self.close(p.h!); try self.close(p.e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
            let (other, od) = try self.store(q), (remote, rd) = try self.store(q); defer { try? FileManager.default.removeItem(at: od); try? FileManager.default.removeItem(at: rd) }
            let denied = try self.pair(q, other, remote, fileGrant: false); XCTAssertEqual(try self.source(denied.h!, denied.hl, input).status, .unsupported)
            try self.close(denied.h!); try self.close(denied.e); XCTAssertEqual(try other.close(), .ok); XCTAssertEqual(try remote.close(), .ok)
        }
    }
    func testDroppedSessionKeepsSourceSafeUntilRelease() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); try self.file(input, size: 262144)
            let p = try self.pair(q, hs, es), source = try XCTUnwrap(self.source(p.h!, p.hl, input).source); try self.ready(source); XCTAssertEqual(try source.send(), .ok)
            for _ in 0..<40 { _ = try source.step(); try self.pump(p) }
            weak var host = p.h; p.h = nil; XCTAssertNil(host)
            q.async {
                do {
                    _ = try source.step(); let value = try source.info(); XCTAssertEqual(value.phase, .uncertain); XCTAssertEqual(value.error, .unknown); XCTAssertFalse(value.transfer.confirmed)
                    XCTAssertEqual(try source.closeStep(), .ok); try self.close(p.e)
                    q.async {
                        do {
                            XCTAssertEqual(try hs.usage().sessions, 0); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
                            try FileManager.default.removeItem(at: hd); try FileManager.default.removeItem(at: ed); try FileManager.default.removeItem(at: input)
                        } catch { XCTFail("source parent reaper: \(error)") }
                    }
                } catch { XCTFail("source after Session drop: \(error)") }
            }
        }
    }
    func testDroppedSourceThenSessionReleasesBothLedgers() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); try self.file(input, size: 262144)
            let p = try self.pair(q, hs, es); var source: Source? = try XCTUnwrap(self.source(p.h!, p.hl, input).source)
            try self.ready(source!); XCTAssertEqual(try source!.send(), .ok); for _ in 0..<30 { _ = try source!.step(); try self.pump(p) }
            source = nil; p.h = nil
            q.async {
                do {
                    XCTAssertEqual(try hs.usage().sessions, 0); try self.close(p.e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
                    try FileManager.default.removeItem(at: hd); try FileManager.default.removeItem(at: ed); try FileManager.default.removeItem(at: input)
                } catch { XCTFail("source and Session destructors: \(error)") }
            }
        }
    }
    func testDroppedSourceIsReapedWhileSessionKeepsRunning() {
        worker { q in
            let (hs, hd) = try self.store(q), (es, ed) = try self.store(q), input = try self.directory(); try self.file(input, size: 262144)
            let p = try self.pair(q, hs, es); var source: Source? = try XCTUnwrap(self.source(p.h!, p.hl, input).source)
            let baseline = Source.pendingCloseCount; try self.ready(source!); XCTAssertEqual(try source!.send(), .ok)
            for _ in 0..<30 { _ = try source!.step(); try self.pump(p) }; source = nil
            q.async {
                do {
                    for _ in 0..<2000 { try self.pump(p); if Source.pendingCloseCount == baseline { break } }
                    XCTAssertEqual(Source.pendingCloseCount, baseline); XCTAssertEqual(try p.h!.info().end, .none)
                    try self.close(p.h!); try self.close(p.e); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
                    try FileManager.default.removeItem(at: hd); try FileManager.default.removeItem(at: ed); try FileManager.default.removeItem(at: input)
                } catch { XCTFail("live Session source reaper: \(error)") }
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
            var source: Source?; var sourceDone = false, resourceReceived = false, resourceReady = false
            let input = try self.directory(); try self.file(input, size: 262144); defer { try? FileManager.default.removeItem(at: input) }
            while try s.info().phase != .ended { _ = try s.step(milliseconds: 5); let i = try s.info(); guard ProcessInfo.processInfo.systemUptime < end else { throw GhostermError.failed("RPC deadline") }
                if i.phase == .consent { XCTAssertEqual(try s.decide(ticket: i.consentToken, decision: .allow), 0) }
                guard i.phase == .active else { continue }; let l = try XCTUnwrap(s.lease())
                if source == nil && !sourceDone { source = try XCTUnwrap(self.source(s, l, input).source) }
                if let active = source {
                    let phase = try active.step()
                    if phase == .ready { XCTAssertEqual(try active.send(), .ok) }
                    if phase == .succeeded {
                        let value = try active.info(); XCTAssertTrue(value.transfer.confirmed); XCTAssertEqual(value.transfer.state, .succeeded)
                        XCTAssertEqual(value.hashed, 262144); XCTAssertEqual(value.readBytes, 262144); XCTAssertEqual(value.accepted, 262144)
                        XCTAssertEqual(try active.closeStep(), .ok); source = nil; sourceDone = true
                    }
                }
                if !resourceReceived { let (st, snap) = try s.takeResource(l); XCTAssertTrue(st == .ok || st == .wait)
                    if let snap { try self.verify(snap, size: 262144); XCTAssertEqual(try snap.close(), .ok); resourceReceived = true } }
                resourceReady = sourceDone && resourceReceived
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
    func testTwoHopBidirectionalSourcesAndRPC() throws { try network("two") }
    func testThreeHopBidirectionalSourcesAndRPC() throws { try network("three") }
}

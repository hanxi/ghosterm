import Cghosterm
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionChannelTests: XCTestCase {
    private func worker(_ body: @escaping (DispatchQueue) throws -> Void) {
        let done = expectation(description: "native channel worker"), queue = DispatchQueue(label: "ghosterm.extension.test")
        queue.async { do { try body(queue) } catch { XCTFail("native channel worker: \(error)") }; queue.async { done.fulfill() } }
        wait(for: [done], timeout: 40)
    }
    private func close(_ c: ExtensionChannel) throws {
        for _ in 0..<32 { if try c.closeStep() { return } }
        XCTFail("native channel remains draining")
    }
    private func pair() throws -> [Int32] {
        var fds: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0 else { throw GhostermError.failed("socketpair") }
        return fds
    }
    func testCopiedTemporaryDataAndBidirectionalIO() {
        worker { owner in
            let fds = try self.pair(), a = try ExtensionChannel(readFD: fds[0], writeFD: fds[0], owner: owner)
            let b = try ExtensionChannel(readFD: fds[1], writeFD: fds[1], owner: owner)
            defer { try? self.close(a); try? self.close(b) }
            let expected = Data((0..<16384).map { UInt8($0 % 251) })
            XCTAssertEqual(try a.write(Data(expected)).count, expected.count)
            XCTAssertEqual(try a.info().sent, 0); XCTAssertEqual(try a.info().buffered, 16384)
            XCTAssertEqual(try a.write(Data([1])).state, .wait)
            _ = try a.step(milliseconds: 20)
            var received = Data()
            while received.count < expected.count {
                let (io, data) = try b.read(milliseconds: 20); XCTAssertTrue(io.state == .data || io.state == .wait)
                received.append(data); _ = try a.step(milliseconds: 0)
            }
            XCTAssertEqual(received, expected); XCTAssertEqual(try a.info().sent, 16384)
            XCTAssertEqual(try b.write(Data([0, 255, 1])).count, 3); _ = try b.step(milliseconds: 20)
            XCTAssertEqual(try a.read(milliseconds: 20).1, Data([0, 255, 1]))
            XCTAssertNil(try a.info().exitStatus)
        }
    }
    func testCloseDropsQueuedBytesAndPostCloseRefuses() {
        worker { owner in
            let fds = try self.pair(), a = try ExtensionChannel(readFD: fds[0], writeFD: fds[0], owner: owner)
            let b = try ExtensionChannel(readFD: fds[1], writeFD: fds[1], owner: owner)
            defer { try? self.close(b) }
            XCTAssertEqual(try a.write(Data([1, 2, 3])).count, 3); try self.close(a)
            XCTAssertEqual(try b.read(milliseconds: 20).0.state, .eof)
            XCTAssertThrowsError(try a.info()); XCTAssertThrowsError(try a.write(Data([1])))
        }
    }
    func testStopFromAnotherThreadAndDeinitCleanup() {
        worker { owner in
            let fds = try self.pair()
            var a: ExtensionChannel? = try ExtensionChannel(readFD: fds[0], writeFD: fds[0], owner: owner)
            let b = try ExtensionChannel(readFD: fds[1], writeFD: fds[1], owner: owner)
            defer { try? self.close(b) }
            let done = DispatchSemaphore(value: 0), channel = a!
            DispatchQueue.global().async { channel.stop(); done.signal() }
            XCTAssertEqual(done.wait(timeout: .now() + 2), .success)
            XCTAssertEqual(try channel.read(milliseconds: 20).0.state, .closed)
            try self.close(channel); a = nil
            var orphan: ExtensionChannel? = try {
                let p = try self.pair(); Darwin.close(p[1]); return try ExtensionChannel(readFD: p[0], writeFD: p[0], owner: owner)
            }()
            XCTAssertNotNil(orphan); orphan = nil
            owner.async { ExtensionChannel.retryPendingClose(on: owner); XCTAssertEqual(ExtensionChannel.pendingCloseCount, 0) }
        }
    }
    func testInvalidDescriptorRetainsOwnership() {
        worker { owner in
            let fd = Darwin.open("/dev/null", O_RDONLY); XCTAssertGreaterThanOrEqual(fd, 0)
            defer { Darwin.close(fd) }
            XCTAssertThrowsError(try ExtensionChannel(readFD: fd, writeFD: fd, owner: owner))
            XCTAssertGreaterThanOrEqual(fcntl(fd, F_GETFD), 0)
        }
    }
    private func fixture() throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_EXT_FIXTURE_PORT"].flatMap(UInt16.init),
              let token = env["GHOSTERM_EXT_FIXTURE_TOKEN"] else { throw XCTSkip("explicit memory fixture required") }
        let fd = socket(AF_INET, SOCK_STREAM, 0); guard fd >= 0 else { throw GhostermError.failed("fixture socket") }
        defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard result == 0 else { throw GhostermError.failed("fixture connect") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let auth = Array((token + "\n").utf8)
        guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture token") }
        var data = Data(), chunk = [UInt8](repeating: 0, count: 4096)
        while true { let n = Darwin.read(fd, &chunk, chunk.count); if n == 0 { break }; guard n > 0, data.count + n <= 1048576 else { throw GhostermError.failed("fixture read") }; data.append(contentsOf: chunk.prefix(n)) }
        guard data.count >= 4 else { throw GhostermError.failed("fixture prefix") }
        let size = data.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        guard size == data.count - 4, let value = try JSONSerialization.jsonObject(with: data.dropFirst(4)) as? [String: Any] else { throw GhostermError.failed("fixture JSON") }
        return value
    }
    private func network(_ label: String, unknown: Bool = false, deny: Bool = false, retry: Bool = false) throws {
        let fixture = try fixture()
        worker { owner in
            let route = try XCTUnwrap(fixture[label] as? [String: Any]), ports = try XCTUnwrap(route["ports"] as? [Int])
            let dir = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TMPDIR"]!).appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false)
            defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("known_hosts").path
            var known = try XCTUnwrap(route["known"] as? String)
            if unknown { known = known.split(separator: "\n").dropLast().joined(separator: "\n") + "\n" }
            try known.write(toFile: path, atomically: true, encoding: .utf8)
            let hops = ports.enumerated().map { i, port in
                ExtensionChannel.Hop(options: SSHOptions(host: "route-hop-\(i).invalid", port: UInt16(port), username: fixture["user"] as! String, knownHostsPath: path),
                    address: i == 0 ? SSHAddress(family: 4, bytes: [127, 0, 0, 1] + Array(repeating: 0, count: 12)) : nil,
                    viaHost: i == 0 ? nil : "127.0.0.1")
            }
            let channel = try ExtensionChannel(hops: hops, bootstrap: fixture["bootstrap"] as! String, owner: owner)
            defer { try? self.close(channel) }
            var accepted = 0; var badChallenge: UInt64?; let deadline = ProcessInfo.processInfo.systemUptime + 25
            while true {
                let phase = try channel.step(milliseconds: 5), info = try channel.info()
                XCTAssertLessThan(ProcessInfo.processInfo.systemUptime, deadline)
                if info.challenge != 0 {
                    XCTAssertNotEqual(try channel.decide(hop: info.hop + 1, challenge: info.challenge, decision: .accept), 0)
                    if info.authentication[Int(info.hop)].phase == .hostkey {
                        XCTAssertEqual(info.authentication[Int(info.hop)].host, .unknown)
                        let result = try channel.decide(hop: info.hop, challenge: info.challenge, decision: deny ? .reject : .accept)
                        XCTAssertEqual(result, deny ? 5 : 0)
                    } else {
                        XCTAssertEqual(info.authentication[Int(info.hop)].phase, .credentials)
                        XCTAssertTrue(info.authentication[Int(info.hop)].host == .known ||
                            (info.authentication[Int(info.hop)].host == .unknown && info.decisions[Int(info.hop)] == .accept))
                        XCTAssertNotEqual(try channel.supply(hop: info.hop, challenge: info.challenge + 1,
                            credential: .privateKey(pem: fixture["pem"] as! String, passphrase: nil)), 0)
                        let bad = retry && info.hop == 0 && badChallenge == nil
                        if bad { badChallenge = info.challenge }
                        else if retry && info.hop == 0 {
                            XCTAssertEqual(info.authentication[0].error, 4)
                            XCTAssertGreaterThan(info.challenge, badChallenge!)
                            XCTAssertNotEqual(try channel.supply(hop: 0, challenge: badChallenge!,
                                credential: .privateKey(pem: fixture["pem"] as! String, passphrase: nil)), 0)
                        }
                        XCTAssertEqual(try channel.supply(hop: info.hop, challenge: info.challenge,
                            credential: .privateKey(pem: fixture[bad ? "wrongPem" : "pem"] as! String, passphrase: nil)), 0)
                        accepted += 1
                    }
                }
                if phase == .ready || phase == .failed { break }
            }
            if deny { XCTAssertEqual(try channel.info().phase, .failed); XCTAssertEqual(try channel.info().error, 5); return }
            XCTAssertEqual(try channel.info().phase, .ready); XCTAssertEqual(accepted, ports.count + (retry ? 1 : 0))
            func read(_ n: Int) throws -> Data { var out = Data(); while out.count < n { let (io, bytes) = try channel.read(capacity: min(n - out.count, 16384), milliseconds: 5); XCTAssertTrue(io.state == .data || io.state == .wait); out.append(bytes); XCTAssertLessThan(ProcessInfo.processInfo.systemUptime, deadline) }; return out }
            XCTAssertEqual(try read(5), Data("ready".utf8))
            for at in stride(from: 0, to: 262144, by: 16384) {
                let value = Data((at..<(at + 16384)).map { UInt8($0 % 251) }); XCTAssertEqual(try channel.write(value).count, value.count)
                while try channel.info().buffered > 0 { _ = try channel.step(milliseconds: 5) }
                XCTAssertEqual(try read(value.count), value)
            }
            XCTAssertEqual(try channel.info().sent, 262144); XCTAssertEqual(try channel.info().received, 262149)
        }
    }
    func testTwoHopSSH() throws { try network("two") }
    func testThreeHopSSH() throws { try network("three") }
    func testUnknownHostRequiresBoundDecision() throws { try network("two", unknown: true) }
    func testExplicitHostRejection() throws { try network("two", unknown: true, deny: true) }
    func testCredentialRetryRejectsPriorChallenge() throws { try network("two", retry: true) }
}

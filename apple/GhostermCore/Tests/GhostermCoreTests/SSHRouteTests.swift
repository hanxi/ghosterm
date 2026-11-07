import XCTest
import Darwin
@testable import GhostermCore

final class SSHRouteTests: XCTestCase {
    func testRouteControlledEndToEnd() throws {
        let env = ProcessInfo.processInfo.environment
        guard let ports = env["GHOSTERM_ROUTE_PORTS"], let known = env["GHOSTERM_ROUTE_KNOWN"],
              let user = env["GHOSTERM_ROUTE_USER"] else { throw XCTSkip("Owned jump fixture required") }
        let pem = String(decoding: FileHandle.standardInput.readDataToEndOfFile(), as: UTF8.self)
        XCTAssertTrue(pem.hasPrefix("-----BEGIN"))
        let owner = DispatchQueue(label: "route.test.owner")
        func sockets() -> Int {
            (try! FileManager.default.contentsOfDirectory(atPath: "/dev/fd")).compactMap(Int32.init).filter {
                var value = stat(); return fstat($0, &value) == 0 && (value.st_mode & S_IFMT) == S_IFSOCK
            }.count
        }
        let before = owner.sync { sockets() }
        let finished = expectation(description: "route IO")
        owner.async {
          defer { finished.fulfill() }
          do {
            let values = ports.split(separator: ",").compactMap { UInt16($0) }
            XCTAssertTrue((2...3).contains(values.count))
            let hops = values.enumerated().map { index, port in
                SSHRoute.Hop(options: SSHOptions(host: "route-hop-\(index).invalid", port: port, username: user, knownHostsPath: known),
                    address: index == 0 ? SSHAddress(family: 4, bytes: [127,0,0,1] + Array(repeating: 0, count: 12)) : nil,
                    viaHost: index == 0 ? nil : "127.0.0.1")
            }
            let route = try SSHRoute(hops: hops, owner: owner)
            XCTAssertNil(route.take())
            let deadline = Date().addingTimeInterval(8)
            var prompted = Set<UInt32>()
            while route.step() != .ready {
                let info = try XCTUnwrap(route.info)
                XCTAssertNotEqual(info.phase, .failed); XCTAssertNotEqual(info.phase, .cancelled)
                guard info.phase != .failed && info.phase != .cancelled && Date() < deadline else { throw NSError(domain: "route", code: 1) }
                if info.phase == .authenticating && info.authentication.phase == .credentials {
                    XCTAssertEqual(info.authentication.host, .known)
                    XCTAssertFalse(info.authentication.fingerprint.isEmpty)
                    XCTAssertTrue(prompted.insert(info.hop).inserted)
                    try route.supply(options: hops[Int(info.hop)].options, credential: .privateKey(pem: pem, passphrase: nil))
                }
                Thread.sleep(forTimeInterval: 0.001)
            }
            XCTAssertEqual(prompted.count, hops.count)
            var session: SSHSession? = try XCTUnwrap(route.take())
            XCTAssertNil(route.take()); route.close() // must not close the transferred owner
            if env["GHOSTERM_ROUTE_MODE"] == "pty" {
                var transport: Transport? = try Transport.openSSHShell(session!, cols: 71, rows: 23,
                    timeoutMilliseconds: 3000, cancellation: SSHCancellation())
                try transport!.write("stty -echo; stty size; printf 'ROUTE_%s\\n' READY\n")
                var output = ""
                for _ in 0..<60 {
                    if case .data(let bytes) = try transport!.read(timeoutMilliseconds: 100) {
                        output += String(decoding: bytes, as: UTF8.self)
                        if output.contains("ROUTE_READY") { break }
                    }
                }
                XCTAssertTrue(output.contains("23 71") && output.contains("ROUTE_READY"))
                transport = nil
                session!.close(); session!.close()
            }
            session = nil // alternate case exercises borrowed-session deinit
          } catch { XCTFail("Route IO failed: \(error)") }
        }
        wait(for: [finished], timeout: 12)
        let deadline = Date().addingTimeInterval(2)
        while owner.sync(execute: { sockets() }) != before && Date() < deadline { Thread.sleep(forTimeInterval: 0.005) }
        XCTAssertEqual(owner.sync { sockets() }, before, "route disposal lost socket ownership")
        print("SSH JUMP IO PASS")
    }
}

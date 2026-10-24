import Foundation
import XCTest
@testable import GhostermCore

final class SessionSetupTests: XCTestCase {
    func testAuthenticatorOwnsFreshCredentialsAndCloses() {
        let done = expectation(description: "setup IO owner")
        let owner = DispatchQueue(label: "dev.ghosterm.test.setup")
        owner.async {
            do {
                let auth = try SessionAuthenticator(options: .init(host: "fixture.invalid", port: 22, username: "fixture", knownHostsPath: "/dev/null"),
                    address: .init(family: 4, bytes: [127,0,0,1] + Array(repeating: 0, count: 12)), credential: .password("fixture-not-secret"), owner: owner)
                XCTAssertEqual(auth.info?.phase, .tcp); XCTAssertEqual(auth.info?.host, .unchecked)
                XCTAssertNil(auth.take()); XCTAssertEqual(auth.decide(.accept), 1)
                auth.close(); auth.close(); XCTAssertNil(auth.info); XCTAssertEqual(auth.step(), .failed)
                XCTAssertThrowsError(try SessionAuthenticator(options: .init(host: "fixture", username: "fixture"),
                    address: .init(family: 4, bytes: [127]), credential: .password("fixture"), owner: owner))
            } catch { XCTFail("\(error)") }
            done.fulfill()
        }
        wait(for: [done], timeout: 5)
    }
    func testCancellationFlagCrossesQueues() {
        let flag = SessionCancellation(); XCTAssertFalse(flag.isCancelled)
        let done = expectation(description: "cancel flag")
        DispatchQueue.global().async { flag.cancel(); flag.cancel(); done.fulfill() }
        wait(for: [done], timeout: 5); XCTAssertTrue(flag.isCancelled)
    }
    func testRealSSHSetup() throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SETUP_HOST"], let port = env["GHOSTERM_SETUP_PORT"].flatMap(UInt16.init),
              let user = env["GHOSTERM_SETUP_USER"], let keyPath = env["GHOSTERM_SETUP_KEY"],
              let work = env["GHOSTERM_SETUP_WORK"], let fingerprint = env["GT_TEST_HOSTKEY_SHA256"] else { throw XCTSkip("requires disposable sshd setup probe") }
        let pem = try String(contentsOfFile: keyPath, encoding: .utf8)
        let done = expectation(description: "real setup SSH")
        let owner = DispatchQueue(label: "dev.ghosterm.test.setup.ssh")
        owner.async {
            do {
                var parsed: RecoverableSession.Address?
                // The native resolver completes on this same owner; it is never waited on by the UI.
                _ = SessionAddressResolver.resolve(host: host, owner: owner) { result in
                    do {
                        parsed = try result.get()
                        let auth = try SessionAuthenticator(options: .init(host: "swift-setup-identity.invalid", port: port, username: user, knownHostsPath: work + "/swift-setup-known"),
                            address: parsed!, credential: .privateKey(pem: pem, passphrase: nil), timeoutMilliseconds: 5000, owner: owner)
                        defer { auth.close() }
                        let deadline = ProcessInfo.processInfo.systemUptime + 8
                        var trust = 0
                        while auth.step() != .ready {
                            guard let i = auth.info, i.phase != .failed, ProcessInfo.processInfo.systemUptime < deadline else { throw GhostermError.failed("原生 SSH 认证未就绪") }
                            if i.phase == .hostkey {
                                XCTAssertEqual(i.host, .unknown); XCTAssertEqual(i.fingerprint, fingerprint)
                                XCTAssertEqual(auth.decide(.accept), 0); trust += 1
                            }
                            Thread.sleep(forTimeInterval: 0.001)
                        }
                        XCTAssertEqual(trust, 1)
                        guard let connection = auth.take() else { throw GhostermError.failed("未交接 SSH") }
                        XCTAssertNil(auth.take()); auth.close()
                        defer { XCTAssertTrue(connection.close()) }
                        let cancellation = SessionCancellation(); var reports = 0
                        let absent = connection.install(bundleDirectory: work + "/absent-catalog", root: "/fixture-no-action", cancellation: cancellation)
                        XCTAssertEqual(absent.status, 1); XCTAssertFalse(absent.published); XCTAssertFalse(absent.publicationUnknown)
                        if env["GHOSTERM_SETUP_EXPECT_LINUX"] == "1" {
                            let info = try connection.probe(cancellation: cancellation) { _ in reports += 1; XCTAssertFalse(connection.close()) }
                            XCTAssertFalse(info.arch.isEmpty); XCTAssertFalse(info.glibc.isEmpty); XCTAssertTrue(info.home.hasPrefix("/"))
                            if let bundle = env["GHOSTERM_SETUP_BUNDLE"], let root = env["GHOSTERM_SETUP_INSTALL_ROOT"] {
                                let installed = connection.install(bundleDirectory: bundle, root: root, cancellation: cancellation) { _ in reports += 1; XCTAssertFalse(connection.close()) }
                                XCTAssertEqual(installed.status, 0); XCTAssertTrue(installed.published); XCTAssertFalse(installed.publicationUnknown)
                                XCTAssertTrue(installed.daemon.hasPrefix(root + "/releases/")); XCTAssertTrue(installed.proxy.hasPrefix(root + "/releases/"))
                                print("PASS native Linux payload install")
                            }
                        } else {
                            XCTAssertThrowsError(try connection.probe(cancellation: cancellation) { _ in reports += 1; XCTAssertFalse(connection.close()) }) { error in
                                guard case GhostermError.status(let code, _) = error else { XCTFail("\(error)"); return }; XCTAssertEqual(code, 7)
                            }
                        }
                        XCTAssertGreaterThan(reports, 0); cancellation.cancel()
                        XCTAssertThrowsError(try connection.probe(cancellation: cancellation)) { error in
                            guard case GhostermError.status(let code, _) = error else { XCTFail("\(error)"); return }; XCTAssertEqual(code, 8)
                        }
                        print("PASS native async resolver/authenticator/take/Linux capability/cancellation")
                    } catch { XCTFail("\(error)") }
                    done.fulfill()
                }
            }
        }
        wait(for: [done], timeout: env["GHOSTERM_SETUP_BUNDLE"] == nil ? 15 : 150)
    }
}

import Foundation
import XCTest
@testable import GhostermCore

final class SSHAddressResolverTests: XCTestCase {
    private func resolve(_ host: String, preferIPv6: Bool = false, timeout: UInt32 = 3000,
                         check: @escaping (Result<SSHAddress, SSHAddressResolver.Failure>) -> Void) {
        let done = expectation(description: "native address completion")
        let owner = DispatchQueue(label: "dev.ghosterm.test.resolver")
        owner.async {
            var returned = false
            _ = SSHAddressResolver.resolve(host: host, preferIPv6: preferIPv6, timeoutMilliseconds: timeout, owner: owner) { result in
                XCTAssertTrue(returned); XCTAssertFalse(Thread.isMainThread); check(result); done.fulfill()
            }
            returned = true
        }
        wait(for: [done], timeout: 5)
    }
    func testIPv4BinaryAddress() {
        resolve("127.0.0.1") { result in
            guard case .success(let a) = result else { XCTFail("\(result)"); return }
            XCTAssertEqual(a.family, 4); XCTAssertEqual(a.bytes, [127,0,0,1] + Array(repeating: 0, count: 12)); XCTAssertEqual(a.scope, 0)
        }
    }
    func testIPv6BinaryAddress() {
        resolve("::1") { result in
            guard case .success(let a) = result else { XCTFail("\(result)"); return }
            XCTAssertEqual(a.family, 6); XCTAssertEqual(a.bytes, Array(repeating: 0, count: 15) + [1]); XCTAssertEqual(a.scope, 0)
        }
    }
    func testScopedIPv6() {
        resolve("fe80::1%1") { result in
            guard case .success(let a) = result else { XCTFail("\(result)"); return }
            XCTAssertEqual(a.family, 6); XCTAssertEqual(a.scope, 1); XCTAssertEqual(a.bytes.prefix(2), [0xfe,0x80])
        }
    }
    func testLocalhostUsesActualAsyncDNS() {
        resolve("localhost") { result in
            guard case .success(let a) = result else { XCTFail("\(result)"); return }
            XCTAssertEqual(a.family, 4); XCTAssertEqual(Array(a.bytes.prefix(4)), [127,0,0,1])
        }
    }
    func testInvalidHostAndDeadlineParameters() {
        resolve("host\0suffix") { result in XCTAssertEqual(result.failure, .invalidHost) }
        resolve("::bad::address") { result in XCTAssertEqual(result.failure, .invalidHost) }
        resolve("127.0.0.1", timeout: 0) { result in XCTAssertEqual(result.failure, .invalidHost) }
        resolve("127.0.0.1", timeout: 60001) { result in XCTAssertEqual(result.failure, .invalidHost) }
    }
    func testCancelRevokesCompletionAndReleasesOwner() {
        let done = expectation(description: "cancelled resolver released")
        let owner = DispatchQueue(label: "dev.ghosterm.test.resolver.cancel")
        owner.async {
            var callbacks = 0
            var resolver: SSHAddressResolver? = SSHAddressResolver.resolve(host: "127.0.0.1", owner: owner) { _ in callbacks += 1 }
            weak var weakResolver = resolver
            resolver?.cancel(); resolver?.cancel(); resolver = nil
            owner.asyncAfter(deadline: .now() + .milliseconds(50)) {
                XCTAssertEqual(callbacks, 0); XCTAssertNil(weakResolver); done.fulfill()
            }
        }
        wait(for: [done], timeout: 5)
    }
}
private extension Result where Failure == SSHAddressResolver.Failure {
    var failure: Failure? { if case .failure(let error) = self { return error }; return nil }
}

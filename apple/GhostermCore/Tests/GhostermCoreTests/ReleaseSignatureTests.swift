import Foundation
import XCTest
@testable import GhostermCore

final class ReleaseSignatureTests: XCTestCase {
    func testDisabledCryptoCapabilityIsExplicit() {
        // The ordinary host Core profile has no OpenSSL dependency. macOS
        // product policy authentication continues to use CryptoKit.
        XCTAssertEqual(ReleaseSignature.verify(publicKey: Data(repeating: 0, count: 32),
            signature: Data(repeating: 0, count: 64), descriptor: Data("{}".utf8)), 7)
    }
    func testBindingRejectsInvalidLengthsBeforeCore() {
        let key = Data(repeating: 0, count: 32), signature = Data(repeating: 0, count: 64)
        for size in [0, 31, 33] {
            XCTAssertEqual(ReleaseSignature.verify(publicKey: Data(repeating: 0, count: size),
                signature: signature, descriptor: Data("{}".utf8)), 1)
        }
        for size in [0, 63, 65] {
            XCTAssertEqual(ReleaseSignature.verify(publicKey: key,
                signature: Data(repeating: 0, count: size), descriptor: Data("{}".utf8)), 1)
        }
        for size in [0, 65537] {
            XCTAssertEqual(ReleaseSignature.verify(publicKey: key, signature: signature,
                descriptor: Data(repeating: 0, count: size)), 1)
        }
    }
}

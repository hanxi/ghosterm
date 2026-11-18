import Foundation
import XCTest
@testable import GhostermAppModel

final class PluginChannelCacheProcessTests: XCTestCase {
    func testCommitOwnedChannelPolicy() throws {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["GHOSTERM_CHANNEL_CACHE_FIXTURE"] else {
            throw XCTSkip("only the explicit subprocess fixture runs this probe")
        }
        let root = try XCTUnwrap(env["TMPDIR"])
        let directory = URL(fileURLWithPath: path).standardizedFileURL
        XCTAssertTrue(directory.path.hasPrefix(URL(fileURLWithPath: root).standardizedFileURL.path + "/"))
        guard directory.path.hasPrefix(URL(fileURLWithPath: root).standardizedFileURL.path + "/") else { return }
        func bytes(_ name: String) throws -> Data {
            try XCTUnwrap(env[name].flatMap { Data(base64Encoded: $0) })
        }
        let verified = try PluginDirectorySignatureVerifier.refreshChannelPolicy(
            statement: bytes("GHOSTERM_CHANNEL_STATEMENT"), signature: bytes("GHOSTERM_CHANNEL_SIGNATURE"),
            policy: bytes("GHOSTERM_CHANNEL_POLICY"), channel: try XCTUnwrap(env["GHOSTERM_CHANNEL_NAME"]),
            channelRoot: bytes("GHOSTERM_CHANNEL_PUBLIC_ROOT"), now: UInt64(Date().timeIntervalSince1970),
            stateDirectory: directory)
        XCTAssertEqual(verified.generation, 2)
    }
}

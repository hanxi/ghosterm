import Foundation
import XCTest
@testable import GhostermAppModel

@MainActor
final class HostTargetIdentityTests: XCTestCase {
    func testUnicodeAliasesResolveToTheirOwnTargets() throws {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("config")
        let a = "caf\u{00e9}", b = "cafe\u{0301}"
        try "Host \(a)\n HostName first.invalid\n User first\n Port 2222\nHost \(b)\n HostName second.invalid\n User second\n Port 3333\n".write(to: path, atomically: false, encoding: .utf8)
        let model = isolatedAppModel(configPath: path.path, knownHostsPath: directory.appendingPathComponent("known_hosts").path, settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        model.reload()
        let first = model.remoteTarget(for: a), second = model.remoteTarget(for: b)
        XCTAssertTrue(first.host == "first.invalid" && first.user == "first" && first.port == 2222
                      && second.host == "second.invalid" && second.user == "second" && second.port == 3333,
                      "selecting the second alias must not borrow the first row's endpoint or credentials")
    }
    func testTargetSelectionDoesNotFoldUnicodeNormalization() {
        XCTAssertFalse(AppModel.Target.host("caf\u{00e9}") == .host("cafe\u{0301}"))
    }
    func testSelectionUsesASCIIHostCaseSemantics() {
        XCTAssertTrue(AppModel.Target.host("WEB") == .host("web"))
        XCTAssertEqual(Set([AppModel.Target.host("WEB"), .host("web"), .localShell]).count, 2)
    }
}

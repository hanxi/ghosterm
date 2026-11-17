import Foundation
import XCTest
@testable import GhostermCore

final class ConfigDirectiveTests: XCTestCase {
    func testLexicalSnapshotOwnsDecodedValuesAndPreservesBytes() throws {
        let text = "Host\tdemo sibling\r\n\tHostName = \"host name\" # keep\r\n IdentityFile \"a,b 中文\"\r\n IdentityFile next\r\n User first\r\n User second\r\n"
        let config = try SSHConfig(contents: text)
        let entries = try config.directives(alias: "DEMO")
        XCTAssertEqual(entries.map(\.keyword), ["hostname", "identityfile", "identityfile", "user", "user"])
        XCTAssertEqual(entries.map(\.arguments), [["host name"], ["a,b 中文"], ["next"], ["first"], ["second"]])
        XCTAssertEqual(entries.map(\.sourceLine), [2, 3, 4, 5, 6])
        XCTAssertEqual(entries[0].raw, "\tHostName = \"host name\" # keep\r\n")
        XCTAssertTrue(entries[1].renderPerLine)
        XCTAssertEqual(Data(config.serialized().utf8), Data(text.utf8))
        try config.edit(alias: "demo", [("User", ["after"])])
        XCTAssertEqual(entries[3].arguments, ["first"])
        XCTAssertEqual(try config.directives(alias: "sibling")[3].arguments, ["after"])
    }

    func testInvalidAndAmbiguousSourcesDoNotLookEmpty() throws {
        XCTAssertThrowsError(try SSHConfig(contents: "Host demo\n User \"bad\n"))
        XCTAssertThrowsError(try SSHConfig(contents: "Host demo\u{0}\n"))
        let duplicate = try SSHConfig(contents: "Host demo\n User first\nHost demo\n User second\n")
        XCTAssertThrowsError(try duplicate.directives(alias: "demo"))
        let conditional = try SSHConfig(contents: "Host demo\n User first\nMatch all\n User conditional\n")
        XCTAssertThrowsError(try conditional.directives(alias: "demo"))
        let empty = try SSHConfig(contents: "Host demo\n")
        XCTAssertEqual(try empty.directives(alias: "demo").count, 0)
        XCTAssertThrowsError(try empty.directives(alias: "missing"))
        XCTAssertThrowsError(try empty.directives(alias: "demo\u{0}suffix"))
    }

    func testLongTokenAndFailedEditPreserveOriginal() throws {
        let token = String(repeating: "字", count: 4096)
        let text = "Host demo\n IdentityFile \(token)\n"
        let config = try SSHConfig(contents: text)
        XCTAssertEqual(try config.directives(alias: "demo")[0].arguments, [token])
        XCTAssertThrowsError(try config.edit(alias: "demo", [("User", ["\"bad"])]))
        XCTAssertThrowsError(try config.edit(alias: "demo", [("User", ["x\nHost injected"])]))
        XCTAssertEqual(Data(config.serialized().utf8), Data(text.utf8))
    }

    func testInvalidUTF8FileIsRefusedBeforeLossyEditing() throws {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let file = directory.appendingPathComponent("config")
        try (Data("Host demo\n User ".utf8) + Data([0xff, 0x0a])).write(to: file)
        XCTAssertThrowsError(try SSHConfig(path: file.path))
    }
}

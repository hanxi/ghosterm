import Foundation
import GhostermCore
@testable import GhostermAppModel
import XCTest

final class HostEditorDraftTests: XCTestCase {
    func testUnchangedSnapshotProducesNoEditsDespiteDuplicatesAndRawStyle() throws {
        let text = "User inherited\r\nHost\tdemo sibling\r\n\tHostName = \"host name\" # retained\r\n User first\r\n User second\r\n IdentityFile \"a,b 中文\"\r\n IdentityFile next\r\n Weird x\r\n Weird y\r\n"
        let model = try HostListModel(contents: text)
        let draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        XCTAssertEqual(draft.hostname.value, "host name")
        XCTAssertEqual(draft.user.value, "first")
        XCTAssertEqual(draft.identities.value, "a,b 中文\nnext")
        XCTAssertFalse(draft.port.enabled)
        XCTAssertNotNil(draft.notice)
        XCTAssertNotNil(draft.extras.first?.readOnlyReason)
        XCTAssertTrue(try draft.edits().isEmpty)
        XCTAssertEqual(Data(model.serialized.utf8), Data(text.utf8))
    }

    func testOnlyDirtyScalarIsPatchedAndInheritanceDoesNotBecomeExplicit() throws {
        let model = try HostListModel(contents: "Host demo\n\tUser first # raw\n IdentityFile \"a,b\"\nHost *\n Port 2222\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        XCTAssertFalse(draft.port.enabled)
        XCTAssertTrue(draft.port.source.contains("2222"))
        draft.user.value = "space #quote\"\\"
        let edits = try draft.edits()
        XCTAssertEqual(edits.map { $0.0 }, ["User"])
        try model.edit(alias: "demo", edits)
        XCTAssertEqual(try model.directiveSnapshot(of: "demo")[0].arguments, ["space #quote\"\\"])
        XCTAssertTrue(model.serialized.contains(" IdentityFile \"a,b\"\nHost *\n Port 2222\n"))
    }

    func testIdentityLinesKeepCommasSpacesAndAllReferences() throws {
        let model = try HostListModel(contents: "Host demo\n IdentityFile one\n IdentityFile two\nHost *\n IdentityFile inherited\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        XCTAssertTrue(draft.identities.source.contains("inherited"))
        draft.identities.value = "a,b\n/keys/中文 空格\nquote\"slash\\"
        try model.edit(alias: "demo", draft.edits())
        XCTAssertEqual(try model.identities(alias: "demo").map(\.value), ["a,b", "/keys/中文 空格", "quote\"slash\\", "inherited"])
    }

    func testInheritanceDeletesOnlyOwnRows() throws {
        let model = try HostListModel(contents: "Host demo\n User own\n IdentityFile own\nHost *\n User inherited\n IdentityFile inherited\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        draft.user.enabled = false
        draft.identities.enabled = false
        try model.edit(alias: "demo", draft.edits())
        XCTAssertEqual(model.hosts.first?.user, "inherited")
        XCTAssertEqual(try model.identities(alias: "demo").map(\.value), ["inherited"])
    }

    func testAdvancedFieldsUseNativeTokensAndGroupRepeatedLines() throws {
        let model = try HostListModel(contents: "Host demo\n LocalForward 1234 first:22\n LocalForward 1235 second:22\n ProxyJump old\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        let index = try XCTUnwrap(draft.extras.firstIndex { $0.field.keyword == "localforward" })
        draft.extras[index].field.value = "1234 \"one two:22\"\n1235 third:22"
        try model.edit(alias: "demo", draft.edits())
        XCTAssertEqual(try model.directiveSnapshot(of: "demo").filter { $0.keyword == "localforward" }.map(\.arguments), [["1234", "one two:22"], ["1235", "third:22"]])
        XCTAssertTrue(model.serialized.contains(" ProxyJump old\n"))
    }

    func testInvalidValuesAndConditionalSnapshotsAreRefused() throws {
        var draft = try HostEditorDraft(model: nil, alias: "", isNew: true)
        draft.alias = "demo"
        draft.port.enabled = true; draft.port.value = "65536"
        XCTAssertThrowsError(try draft.edits())
        draft.port.enabled = false; draft.user.enabled = true; draft.user.value = "x\nHost injected"
        XCTAssertThrowsError(try draft.edits())
        let conditional = try HostListModel(contents: "Host demo\n User own\nMatch all\n User conditional\n")
        XCTAssertThrowsError(try HostEditorDraft(model: conditional, alias: "demo", isNew: false))
        let duplicate = try HostListModel(contents: "Host demo\nHost DEMO\n")
        XCTAssertThrowsError(try HostEditorDraft(model: duplicate, alias: "demo", isNew: false))
    }

    func testPatternListDeduplicatesASCIIOnlyAndExcludesNegation() throws {
        let composed = "caf\u{00e9}", decomposed = "cafe\u{0301}"
        let model = try HostListModel(contents: "Host demo DEMO !hidden * \(composed) \(decomposed)\n")
        XCTAssertEqual(model.hosts.count, 3)
        XCTAssertEqual(Set(model.hosts.map(\.id)).count, 3)
        XCTAssertTrue(model.wildcardPatterns.contains("!hidden"))
    }

    func testUnicodeNormalizationDifferenceIsStillDirty() throws {
        let model = try HostListModel(contents: "Host demo\n User caf\u{00e9}\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        draft.user.value = "cafe\u{0301}"
        XCTAssertEqual(try draft.edits().map { $0.0 }, ["User"])
    }
    func testScalarQuotingKeepsOneDecodedArgument() throws {
        let model = try HostListModel(contents: "Host demo\n User before\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        draft.user.value = "two words"
        try model.edit(alias: "demo", draft.edits())
        XCTAssertEqual(try model.directiveSnapshot(of: "demo")[0].arguments, ["two words"])
    }

    func testNegatedPatternsCannotBecomeConnectableHosts() throws {
        let model = try HostListModel(contents: "Host demo !hidden\n")
        XCTAssertEqual(model.hosts.map(\.pattern), ["demo"])
    }

    func testRepeatedAliasesProduceOneConnectableRow() throws {
        let model = try HostListModel(contents: "Host demo DEMO\nHost demo\n")
        XCTAssertEqual(model.hosts.map(\.pattern), ["demo"])
    }

    func testDisabledUnchangedInheritanceDoesNotWriteAfterToggleRoundtrip() throws {
        let model = try HostListModel(contents: "Host demo\n")
        var draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        draft.user.enabled = true; draft.user.value = "unused"; draft.user.enabled = false
        XCTAssertTrue(try draft.edits().isEmpty)
    }

    func testIncludedHostIsExplicitlyReadOnly() throws {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let child = directory.appendingPathComponent("child"), main = directory.appendingPathComponent("config")
        try "Host demo\n User included\n".write(to: child, atomically: false, encoding: .utf8)
        try "Include \(child.path)\n".write(to: main, atomically: false, encoding: .utf8)
        let model = try HostListModel(path: main.path)
        let draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        XCTAssertTrue(draft.readOnlyReason?.contains(child.path) == true)
    }

    func testUnchangedPlainSnapshotProducesNoEdits() throws {
        let model = try HostListModel(contents: "Host demo\n User before\n")
        let draft = try HostEditorDraft(model: model, alias: "demo", isNew: false)
        XCTAssertTrue(try draft.edits().isEmpty)
    }

}

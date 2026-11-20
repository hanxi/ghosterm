import AppKit
import CryptoKit
import Darwin
import Foundation
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class HostEditorUITests: XCTestCase {
    private func runForm(original: String, action: (NSHostingView<HostEditorSheet>, AppModel, URL) async throws -> Void) async throws {
        TestWindows.enterAutomationMode()
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("host-form-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("config")
        try Data(original.utf8).write(to: file)
        let model = isolatedAppModel(configPath: file.path, knownHostsPath: root.appendingPathComponent("known_hosts").path)
        model.reload()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let view = NSHostingView(rootView: HostEditorSheet(model: model, alias: "demo", isNew: false))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 590, height: 740), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = view; TestWindows.presentOffscreen(window)
        defer {
            window.makeFirstResponder(nil)
            window.orderOut(nil)
            window.contentView = nil
            window.close()
        }
        for _ in 0..<100 {
            view.layoutSubtreeIfNeeded()
            if node(view, "host-editor-alias") != nil { break }
            try await Task.sleep(for: .milliseconds(20))
        }
        try await action(view, model, file)
    }

    private func node(_ view: NSView, _ id: String) -> NSObject? {
        view.layoutSubtreeIfNeeded()
        var visited: Set<ObjectIdentifier> = []
        func visit(_ value: NSObject) -> NSObject? {
            guard visited.count < 4096, visited.insert(ObjectIdentifier(value)).inserted else { return nil }
            let name = NSSelectorFromString("accessibilityIdentifier")
            if value.responds(to: name), value.perform(name)?.takeUnretainedValue() as? String == id { return value }
            let children = NSSelectorFromString("accessibilityChildren")
            if value.responds(to: children), let list = value.perform(children)?.takeUnretainedValue() as? [NSObject] {
                for child in list { if let result = visit(child) { return result } }
            }
            return nil
        }
        return visit(view)
    }
    private func press(_ value: NSObject) -> Bool {
        let selector = NSSelectorFromString("accessibilityPerformPress")
        guard value.responds(to: selector), let implementation = value.method(for: selector) else { return false }
        typealias Press = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Press.self)(value, selector)
    }
    private func change(_ value: NSObject, text: String) throws {
        value.perform(NSSelectorFromString("setAccessibilityValue:"), with: text)
        let native = try XCTUnwrap((value as? NSTextField) ?? (value as? NSCell)?.controlView as? NSTextField)
        NotificationCenter.default.post(name: NSControl.textDidChangeNotification, object: native)
    }

    func testActualUnchangedSaveLeavesBytesAndBackupUntouched() async throws {
        let text = "Host\tdemo\r\n\tUser first # keep\r\n User second\r\n IdentityFile \"a,b\"\r\n IdentityFile other\r\n"
        try await runForm(original: text) { view, _, file in
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-save"))))
            try await Task.sleep(for: .milliseconds(50))
            XCTAssertTrue((try? Data(contentsOf: file)) == Data(text.utf8) && !FileManager.default.fileExists(atPath: file.deletingPathExtension().appendingPathExtension("bak").path))
        }
    }
    func testActualRenameAndUserEditPreserveUntouchedIdentityRows() async throws {
        try await runForm(original: "Host demo sibling\n User before\n IdentityFile \"a,b\"\n IdentityFile other\n") { view, model, file in
            try self.change(XCTUnwrap(self.node(view, "host-editor-alias")), text: "renamed")
            try self.change(XCTUnwrap(self.node(view, "host-editor-User")), text: "after")
            try await Task.sleep(for: .milliseconds(50))
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-save"))))
            try await Task.sleep(for: .milliseconds(50))
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-confirm-save"))))
            try await Task.sleep(for: .milliseconds(50))
            XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), "Host renamed sibling\n User \"after\"\n IdentityFile \"a,b\"\n IdentityFile other\n")
            XCTAssertTrue(model.hosts.contains { $0.pattern == "renamed" })
        }
    }
    func testActualCancelDoesNotWriteEditedDraft() async throws {
        let text = "Host demo\n User before\n"
        try await runForm(original: text) { view, _, file in
            try self.change(XCTUnwrap(self.node(view, "host-editor-User")), text: "after")
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-cancel"))))
            try await Task.sleep(for: .milliseconds(50))
            XCTAssertEqual(try Data(contentsOf: file), Data(text.utf8))
        }
    }
    private func ownedImportSource(mode: mode_t = 0o600) throws -> (String, Data) {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("key-ui-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let file = directory.appendingPathComponent("identity")
        let bytes = Data("-----BEGIN RSA PRIVATE KEY-----\nNOT_A_CRYPTOGRAPHIC_KEY\n-----END RSA PRIVATE KEY-----\n".utf8)
        try bytes.write(to: file); XCTAssertEqual(chmod(file.path, mode), 0)
        addTeardownBlock {
            KeychainStore.removePrivateKeyPEM(forPath: file.path)
            var missing = false
            do { _ = try KeychainStore.privateKeyPEM(forPath: file.path) }
            catch KeychainStoreError.notFound { missing = true }
            catch { XCTFail("owned Keychain UI fixture cleanup failed") }
            XCTAssertTrue(missing)
            try? FileManager.default.removeItem(at: directory)
        }
        return (file.path, bytes)
    }

    func testActualKeyImportButtonStoresMaterialWithoutSavingTheForm() async throws {
        let (path, bytes) = try ownedImportSource()
        let original = "Host demo\n IdentityFile \"\(path)\"\n"
        try await runForm(original: original) { view, _, file in
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-import-key"))))
            for _ in 0..<250 {
                if self.node(view, "host-editor-import-result") != nil || self.node(view, "host-editor-error") != nil { break }
                try await Task.sleep(for: .milliseconds(20))
            }
            XCTAssertNotNil(self.node(view, "host-editor-import-result"))
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-cancel"))))
            let stored = try? KeychainStore.privateKeyPEM(forPath: path)
            XCTAssertTrue(Data(SHA256.hash(data: Data((stored ?? "").utf8))) == Data(SHA256.hash(data: bytes)) &&
                (try? Data(contentsOf: file)) == Data(original.utf8) &&
                (try? Data(contentsOf: URL(fileURLWithPath: path))) == bytes,
                "import independently persists to Keychain; cancelling config editing must not write either file")
        }
    }

    func testActualKeyImportFailureIsVisibleWithoutSaving() async throws {
        let (path, _) = try ownedImportSource(mode: 0o644)
        let original = "Host demo\n IdentityFile \"\(path)\"\n"
        try await runForm(original: original) { view, _, file in
            XCTAssertTrue(self.press(try XCTUnwrap(self.node(view, "host-editor-import-key"))))
            for _ in 0..<250 {
                if self.node(view, "host-editor-error") != nil { break }
                try await Task.sleep(for: .milliseconds(20))
            }
            XCTAssertTrue(self.node(view, "host-editor-error") != nil && (try? Data(contentsOf: file)) == Data(original.utf8),
                          "a failed private source import must remain visible and leave the form uncommitted")
        }
    }

}

import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor final class PluginFormRPCAppTests: XCTestCase {
    func testTwoLocalFormsKeepRepliesAndCancellationInTheirOriginalRuntime() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let binary = env["GHOSTERM_FORM_GTERM"], let packagePath = env["GHOSTERM_FORM_PACKAGE"],
              let scratch = env["GHOSTERM_FORM_SCRATCH"] else {
            throw XCTSkip("requires the owned QuickJS form fixture")
        }
        XCTAssertEqual(ExtensionRuntime.supportStatus, 0)
        let root = URL(fileURLWithPath: scratch), package = URL(fileURLWithPath: packagePath)
        let binaryDigest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: binary)))
            .map { String(format: "%02x", $0) }.joined()
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }

        func start(_ name: String) async throws -> ExtensionInstanceModel {
            let work = root.appendingPathComponent(name)
            try FileManager.default.createDirectory(at: work, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            let stage = work.appendingPathComponent("stage")
            try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1],
                runtimes: ["portable-js"], capabilities: ["session.rpc", "ui.form"]),
                localJournalDirectory: work.appendingPathComponent("journal"))
            let model = ExtensionInstanceModel(controller: controller)
            addTeardownBlock { await model.stopExtensionRun(); let closed = await controller.close(); XCTAssertTrue(closed) }
            let review = try await controller.review(directory: package)
            XCTAssertEqual(Set(review.requiredCapabilities), ["session.rpc", "ui.form"])
            let approved = try await controller.approve(id: review.id, digest: review.digest)
            XCTAssertEqual(approved, 0)
            let launch = ExtensionLocalLaunch(executableSHA256: binaryDigest,
                workingDirectory: work.path, temporaryDirectory: stage.path, authorized: true)
            await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "form",
                arguments: [], sshAlias: "", executable: binary, executorDirectory: package.path,
                executorDigest: review.digest, methods: ["form.status", "form.cancel"], hostMethods: [], localLaunch: launch)
            if let issue = model.extensionRunIssue { throw GhostermError.failed(issue) }
            XCTAssertEqual(model.extensionRun?.hostMethods, [PluginFormRequest.method])
            await model.decideExtensionConsent(allow: true)
            try await until { model.pendingPluginForm != nil || model.extensionRunIssue != nil }
            if let issue = model.extensionRunIssue { throw GhostermError.failed(issue) }
            return model
        }

        func status(_ model: ExtensionInstanceModel) async throws -> String {
            let bytes = try await model.callPluginViewOutcome("form.status", inputBytes: Data("{}".utf8), command: "form")
            return try XCTUnwrap(PluginViewValue.success(bytes)["status"] as? String)
        }

        let first = try await start("first"), second = try await start("second")
        let a = try XCTUnwrap(first.pendingPluginForm), b = try XCTUnwrap(second.pendingPluginForm)
        XCTAssertEqual(a.request.id, b.request.id)
        XCTAssertNotEqual(a.id, b.id)
        let pane = NSHostingView(rootView: ExtensionRunView(model: first).background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1280, height: 1600)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = pane
        TestWindows.presentOffscreen(window)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(100))
        let words = try PluginTableRendering.words(pane).joined(separator: " ")
        XCTAssertTrue(words.contains("Choose sample settings"), words)

        let answer: PluginFormRequest.Answer = .submitted(["name": .text("甲"), "summary": .toggle(true), "layout": .text("compact")])
        await second.answerPluginForm(id: a.id, answer: answer)
        XCTAssertEqual(second.pendingPluginForm?.id, b.id)
        await first.answerPluginForm(id: a.id, answer: .submitted(["layout": .text("invalid")]))
        XCTAssertEqual(first.pendingPluginForm?.id, a.id)
        await first.answerPluginForm(id: a.id, answer: answer)
        XCTAssertNil(first.pendingPluginForm)
        let firstResult = try await status(first), secondResult = try await status(second)
        XCTAssertEqual(firstResult, "submitted")
        XCTAssertEqual(secondResult, "pending")

        _ = try await second.callPluginViewOutcome("form.cancel", inputBytes: Data("{}".utf8), command: "form")
        try await until { second.pendingPluginForm == nil }
        await second.answerPluginForm(id: b.id, answer: answer)
        let cancelled = try await status(second)
        XCTAssertEqual(cancelled, "cancelled")

        let third = try await start("third")
        pane.rootView = ExtensionRunView(model: third).background(Color(nsColor: .windowBackgroundColor))
        func dismissButton() -> NSObject? {
            pane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(pane).first {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-form-dismiss"
            }
        }
        try await until { dismissButton() != nil }
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(dismissButton())))
        try await until { third.pendingPluginForm == nil }
        let dismissed = try await status(third)
        XCTAssertEqual(dismissed, "dismissed")

        let textOnly = try PluginFormRequest.decode(Data(
            #"{"requestId":"1","title":"Name","fields":[{"kind":"text","id":"name","label":"Name","required":true,"maxLength":3}]}"#.utf8))
        var submitted: PluginFormRequest.Answer?
        let textPane = NSHostingView(rootView: PluginFormView(
            request: textOnly, pluginName: "org.ghosterm.form", answering: false) { submitted = $0 })
        textPane.frame = pane.frame
        window.contentView = textPane
        textPane.layoutSubtreeIfNeeded()
        func editableField(in view: NSView) -> NSTextField? {
            if let field = view as? NSTextField, field.isEditable { return field }
            return view.subviews.lazy.compactMap { editableField(in: $0) }.first
        }
        func submitButton() -> NSObject? {
            textPane.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(textPane).first {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-form-submit"
            }
        }
        try await until { editableField(in: textPane) != nil && submitButton() != nil }
        let field = try XCTUnwrap(editableField(in: textPane))
        XCTAssertTrue(window.makeFirstResponder(field))
        let editor = try XCTUnwrap(field.currentEditor() as? NSTextView)
        editor.insertText("甲乙丙丁", replacementRange: editor.selectedRange())
        try await Task.sleep(for: .milliseconds(80))
        _ = PluginTableRendering.press(try XCTUnwrap(submitButton()))
        XCTAssertNil(submitted)
        editor.selectAll(nil)
        editor.insertText("甲乙丙", replacementRange: editor.selectedRange())
        try await Task.sleep(for: .milliseconds(80))
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(submitButton())))
        XCTAssertEqual(submitted, .submitted(["name": .text("甲乙丙")]))
    }

    private func until(_ predicate: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 12
        while !predicate() && ProcessInfo.processInfo.systemUptime < end {
            try await Task.sleep(for: .milliseconds(20))
        }
        guard predicate() else { throw GhostermError.failed("Plugin form observation timed out") }
    }
}

import AppKit
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor
final class ClaudeConversationViewTests: XCTestCase {
    private let enhancedAccessibility = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")

    private func control(_ id: String, in probe: ConversationUIProbe) -> NSObject? {
        probe.root.layoutSubtreeIfNeeded()
        return PluginTableRendering.elements(probe.root).first {
            PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
        }
    }

    private func press(_ id: String, in probe: ConversationUIProbe) async throws {
        try await wait {
            guard let node = self.control(id, in: probe) else { return false }
            return self.isEnabled(node) == true
        }
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(control(id, in: probe))), id)
    }

    private func isEnabled(_ node: NSObject) -> Bool? {
        let selector = NSSelectorFromString("isAccessibilityEnabled")
        guard node.responds(to: selector), let implementation = node.method(for: selector) else { return nil }
        typealias Enabled = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Enabled.self)(node, selector)
    }

    private func wait(_ predicate: @escaping () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        guard predicate() else { XCTFail("Native conversation state did not arrive"); throw ClaudeRPCClient.Failure.invalidResponse }
    }
    func testRenderedStartSendAndToolApprovalUseTheProductionControls() async throws {
        TestWindows.enterAutomationMode()
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhancedAccessibility)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhancedAccessibility) }
        let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
        let probe = ConversationUIProbe(root: NSHostingView(rootView: ClaudeConversationView(model: model).padding().background(Color(nsColor: .windowBackgroundColor))))
        defer { model.invalidate(); fixture.release(); probe.close() }
        try await press("plugin-claude-start", in: probe); try await wait { model.phase == .ready }
        model.draft = "Native control fixture"; try await press("plugin-claude-send", in: probe)
        try await wait { model.phase == .running }
        fixture.prompt(); try await wait { model.pending.count == 1 }
        try await wait { probe.textViews().contains { $0.identifier?.rawValue == "plugin-claude-tool-input" } }
        let text = try XCTUnwrap(probe.textViews().first { $0.identifier?.rawValue == "plugin-claude-tool-input" })
        XCTAssertTrue(text.string.contains(String(repeating: "中😀", count: 1200)))
        XCTAssertFalse(text.isEditable); XCTAssertTrue(text.isSelectable)
        try await press("plugin-claude-approval-allow", in: probe); try await wait { model.pending.isEmpty }
        XCTAssertEqual(fixture.calls.filter { $0.method == "approve" }.count, 1, "Rendered allow must dispatch one explicit approval")
        XCTAssertEqual(fixture.calls.first { $0.method == "approve" }?.input["allow"]?.boolean, true, "Rendered allow must not send a decline")
    }

    func testRenderedQuestionRequiresSelectionBeforeSubmittingAnAnswer() async throws {
        TestWindows.enterAutomationMode()
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhancedAccessibility)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhancedAccessibility) }
        let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
        let probe = ConversationUIProbe(root: NSHostingView(rootView: ClaudeConversationView(model: model).padding().background(Color(nsColor: .windowBackgroundColor))))
        defer { model.invalidate(); fixture.release(); probe.close() }
        await model.start(); try await wait { model.phase == .ready }; model.draft = "question fixture"; await model.send()
        fixture.prompt(questions: [ClaudeChatFixture.question("Choose a format")]); try await wait { model.pending.count == 1 }
        let submit = "plugin-claude-approval-allow"
        try await wait { self.control(submit, in: probe) != nil }
        XCTAssertEqual(isEnabled(try XCTUnwrap(control(submit, in: probe))), false)
        XCTAssertFalse(fixture.calls.contains { $0.method == "answer" }, "An untouched question must not submit a default answer")
        try await press("plugin-claude-question-0-option-0", in: probe)
        try await press(submit, in: probe)
        try await wait { model.pending.isEmpty }
        XCTAssertEqual(fixture.calls.first { $0.method == "answer" }?.input["answers"]?["Choose a format"]?.text, "Blue",
                       "Rendered choice must preserve the explicit answer")
        XCTAssertFalse(fixture.calls.contains { $0.method == "approve" })
    }
}

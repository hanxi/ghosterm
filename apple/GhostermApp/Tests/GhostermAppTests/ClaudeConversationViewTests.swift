import AppKit
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

@MainActor
final class ClaudeConversationViewTests: XCTestCase {
    private func wait(_ predicate: @escaping () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        guard predicate() else { XCTFail("Native conversation state did not arrive"); throw ClaudeRPCClient.Failure.invalidResponse }
    }
    func testRenderedStartSendAndToolApprovalUseTheProductionControls() async throws {
        let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
        let probe = ConversationUIProbe(root: NSHostingView(rootView: ClaudeConversationView(model: model).padding().background(Color(nsColor: .windowBackgroundColor))))
        defer { model.invalidate(); fixture.release(); probe.close() }
        try await probe.press("启动Claude对话"); try await wait { model.phase == .ready }
        model.draft = "Native control fixture"; try await probe.press("发送")
        try await wait { model.phase == .running }
        fixture.prompt(); try await wait { model.pending.count == 1 }
        try await wait { probe.textViews().contains { $0.identifier?.rawValue == "plugin-claude-tool-input" } }
        let text = try XCTUnwrap(probe.textViews().first { $0.identifier?.rawValue == "plugin-claude-tool-input" })
        XCTAssertTrue(text.string.contains(String(repeating: "中😀", count: 1200)))
        XCTAssertFalse(text.isEditable); XCTAssertTrue(text.isSelectable)
        try await probe.press("批准一次"); try await wait { model.pending.isEmpty }
        XCTAssertEqual(fixture.calls.filter { $0.method == "approve" }.count, 1, "Rendered allow must dispatch one explicit approval")
        XCTAssertEqual(fixture.calls.first { $0.method == "approve" }?.input["allow"]?.boolean, true, "Rendered allow must not send a decline")
    }

    func testRenderedQuestionRequiresSelectionBeforeSubmittingAnAnswer() async throws {
        let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
        let probe = ConversationUIProbe(root: NSHostingView(rootView: ClaudeConversationView(model: model).padding().background(Color(nsColor: .windowBackgroundColor))))
        defer { model.invalidate(); fixture.release(); probe.close() }
        await model.start(); try await wait { model.phase == .ready }; model.draft = "question fixture"; await model.send()
        fixture.prompt(questions: [ClaudeChatFixture.question("Choose a format")]); try await wait { model.pending.count == 1 }
        try await probe.press("提交回答")
        XCTAssertFalse(fixture.calls.contains { $0.method == "answer" }, "An untouched question must not submit a default answer")
        try await probe.press("Blue"); try await probe.press("提交回答")
        try await wait { model.pending.isEmpty }
        XCTAssertEqual(fixture.calls.first { $0.method == "answer" }?.input["answers"]?["Choose a format"]?.text, "Blue",
                       "Rendered choice must preserve the explicit answer")
        XCTAssertFalse(fixture.calls.contains { $0.method == "approve" })
    }
}

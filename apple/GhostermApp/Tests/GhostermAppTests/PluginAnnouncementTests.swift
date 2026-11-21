import AppKit
import SwiftUI
@testable import GhostermApp
import XCTest

@MainActor final class PluginAnnouncementTests: XCTestCase {
    func testPreexistingFormStatusAndSubsequentChangesAreAnnounced() async throws {
        let model = Status()
        var posted: [String] = []
        var otherPosts: [String] = []
        let window = PluginTableWindow(contentRect: NSRect(x: 0, y: 0, width: 500, height: 300),
                                       styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = NSHostingView(rootView: Fixture(model: model, post: { posted.append($0) },
                                                           postOther: { otherPosts.append($0) }))
        TestWindows.presentOffscreen(window)
        defer { window.close() }

        try await until { posted.count == 1 }
        XCTAssertEqual(posted, ["插件请求输入：Sample form"])
        XCTAssertTrue(otherPosts.isEmpty)

        model.message = "正在发送表单答复…"
        try await until { posted.count == 2 && otherPosts.count == 1 }
        XCTAssertEqual(posted, ["插件请求输入：Sample form", "正在发送表单答复…"])
        XCTAssertEqual(otherPosts, ["正在发送表单答复…"])

        model.message = nil
        // Allow SwiftUI to deliver the nil transition before the next request.
        try await Task.sleep(for: .milliseconds(80))
        model.message = "插件请求输入：Another form"
        try await until { posted.count >= 3 && otherPosts.count >= 2 }
        XCTAssertEqual(posted, ["插件请求输入：Sample form", "正在发送表单答复…", "插件请求输入：Another form"])
        XCTAssertEqual(otherPosts, ["正在发送表单答复…", "插件请求输入：Another form"])
    }

    func testTaskCancellationAnnouncesUncertaintyUntilExplicitReadConfirms() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let fixture = PluginTaskFixture()
        let model = fixture.model()
        await model.load()
        fixture.pauseMethod = "view.task.cancel"
        fixture.unknownMethod = "view.task.cancel"
        var posted: [String] = []
        let host = NSHostingView(rootView: PluginTaskView(model: model, announcementObserver: { posted.append($0) }))
        host.frame = NSRect(x: 0, y: 0, width: 600, height: 430)
        let window = PluginTableWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer {
            fixture.release()
            window.close()
        }
        func button(_ id: String) -> NSObject? {
            host.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(host).first {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
            }
        }
        let pending = "Prime task，正在发送取消请求…"
        let unknown = "Prime task，取消结果不明，不会再次发送。请显式读取任务状态。"
        let cancelled = "Prime task，已确认取消"
        try await until { button("plugin-task-cancel") != nil }
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(button("plugin-task-cancel"))))
        try await until { fixture.barrier != nil && posted.contains(pending) }
        fixture.release()
        try await until { posted.contains(unknown) }
        XCTAssertEqual(posted, [pending, unknown])
        XCTAssertFalse(model.canCancel)

        fixture.finish()
        fixture.pauseMethod = "view.task.read"
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(button("plugin-task-read"))))
        try await until { fixture.barrier != nil }
        XCTAssertEqual(posted, [pending, unknown])
        fixture.release()
        try await until { posted.contains(cancelled) }
        XCTAssertEqual(posted, [pending, unknown, cancelled])
        XCTAssertEqual(fixture.calls.filter { $0.0 == "view.task.cancel" }.count, 1)
    }

    private func until(_ ready: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while !ready() && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertTrue(ready(), "Expected status notification callbacks")
    }

    @MainActor private final class Status: ObservableObject {
        @Published var message: String? = "插件请求输入：Sample form"
    }

    private struct Fixture: View {
        @ObservedObject var model: Status
        let post: (String) -> Void
        let postOther: (String) -> Void

        var body: some View {
            VStack {
                Text("Form").announcePluginStatus(model.message, onAppearance: true, post: post)
                Text("Other plugin view").announcePluginStatus(model.message, post: postOther)
            }
        }
    }
}

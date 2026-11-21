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

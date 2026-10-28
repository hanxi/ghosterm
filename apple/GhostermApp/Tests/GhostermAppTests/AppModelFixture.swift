// 普通入口夹具不启动 daemon，也不依赖 runtime catalog。
import AppKit
import Darwin
import Foundation
import GhostermAppModel
import XCTest

@MainActor private final class AppModelFixture: NSObject, XCTestObservation {
    static let shared = AppModelFixture()
    let root: URL
    private override init() {
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        project = project.appendingPathComponent("build").resolvingSymlinksInPath().deletingLastPathComponent()
        root = project.appendingPathComponent("tmp/app-model-" + String(UUID().uuidString.prefix(8)))
        super.init()
        do {
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        } catch { XCTFail("owned application session fixture failed: \(error)") }
        XCTestObservationCenter.shared.addTestObserver(self)
    }
    nonisolated func testBundleDidFinish(_ testBundle: Bundle) {
        MainActor.assumeIsolated {
            try? FileManager.default.removeItem(at: root)
        }
    }
}

@MainActor func isolatedAppModel(configPath: String? = nil, knownHostsPath: String? = nil, settings: SettingsStore? = nil) -> AppModel {
    AppModel(configPath: configPath, knownHostsPath: knownHostsPath, settings: settings)
}

@MainActor func sessionTestDirectory() -> URL { AppModelFixture.shared.root.appendingPathComponent("profile-" + UUID().uuidString) }

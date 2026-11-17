// 普通入口夹具不启动 daemon，也不依赖 runtime catalog。
import AppKit
import Darwin
import Foundation
import GhostermAppModel
import XCTest

@MainActor private final class AppModelFixture: NSObject, XCTestObservation {
    static let shared = AppModelFixture()
    let root: URL
    private var defaultsSuites: [String] = []

    func makeDefaults() -> UserDefaults {
        let suite = "Ghosterm.AppModelFixture." + UUID().uuidString
        defaultsSuites.append(suite)
        return UserDefaults(suiteName: suite)!
    }
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
    // Only the gate's explicit disposable key may be retired. Never infer
    // permission from an arbitrary GHOSTERM_SSH_KEY or scan the user's Keychain.
    nonisolated func testCaseDidFinish(_ testCase: XCTestCase) {
        guard let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_KEYCHAIN_OWNED_REFERENCE"],
              path == ProcessInfo.processInfo.environment["GHOSTERM_SSH_KEY"] else { return }
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let file = URL(fileURLWithPath: path).standardizedFileURL
        let expectedRoot = project.appendingPathComponent("tmp").standardizedFileURL.path + "/"
        guard file.path.hasPrefix(expectedRoot), file.lastPathComponent == "clientkey",
              file.deletingLastPathComponent().lastPathComponent.hasPrefix("apple-app-") else {
            XCTFail("owned Keychain fixture reference is outside the gate's disposable directory")
            return
        }
        KeychainStore.removePrivateKeyPEM(forPath: path)
        do {
            _ = try KeychainStore.privateKeyPEM(forPath: path)
            XCTFail("owned Keychain fixture record was not retired")
        } catch KeychainStoreError.notFound { }
        catch { XCTFail("owned Keychain fixture cleanup was not confirmed: \(error)") }
    }

    nonisolated func testBundleDidFinish(_ testBundle: Bundle) {
        MainActor.assumeIsolated {
            for suite in defaultsSuites {
                UserDefaults(suiteName: suite)?.removePersistentDomain(forName: suite)
            }
            try? FileManager.default.removeItem(at: root)
        }
    }
}

@MainActor func isolatedAppModel(configPath: String? = nil, knownHostsPath: String? = nil, settings: SettingsStore? = nil,
                                pluginResourceDirectory: URL? = nil) -> AppModel {
    let resources = pluginResourceDirectory ?? AppModelFixture.shared.root.appendingPathComponent("plugin-resources")
    do { try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: true) }
    catch { XCTFail("owned plugin resource fixture failed: \(error)") }
    return AppModel(configPath: configPath, knownHostsPath: knownHostsPath, settings: settings,
             defaults: AppModelFixture.shared.makeDefaults(), pluginResourceDirectory: resources)
}

@MainActor func sessionTestDirectory() -> URL { AppModelFixture.shared.root.appendingPathComponent("profile-" + UUID().uuidString) }

// The production entry is exercised unchanged. Tests inject paths into a
// private profile and one explicitly owned daemon per test, never user sessions.
import AppKit
import Darwin
import Foundation
import GhostermAppModel
import XCTest

@MainActor private final class AppModelFixture: NSObject, XCTestObservation {
    static let shared = AppModelFixture()
    let root: URL
    private var daemon: Process?
    private var generation = String(UUID().uuidString.prefix(8))
    var runtimeRoot: URL { root.appendingPathComponent(generation) }
    private var project: URL {
        var path = URL(fileURLWithPath: #filePath); for _ in 0..<5 { path.deleteLastPathComponent() }
        return path.appendingPathComponent("build").resolvingSymlinksInPath().deletingLastPathComponent()
    }
    private override init() {
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        project = project.appendingPathComponent("build").resolvingSymlinksInPath().deletingLastPathComponent()
        root = project.appendingPathComponent("tmp/app-model-" + String(UUID().uuidString.prefix(8)))
        super.init()
        do {
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        } catch { XCTFail("owned application session fixture failed: \(error)") }
        XCTestObservationCenter.shared.addTestObserver(self)
    }
    func ensureRuntime() {
        guard daemon == nil else { return }
        do {
            let runtime = project.appendingPathComponent("build/runtime")
            let data = try Data(contentsOf: runtime.appendingPathComponent("catalog.json"))
            let index = try XCTUnwrap(try JSONSerialization.jsonObject(with: data) as? [String: Any])
            let contract = try XCTUnwrap(index["contract"] as? String), release = try XCTUnwrap(index["local"] as? String)
            let process = Process()
            process.executableURL = runtime.appendingPathComponent("macos-arm64/" + release + "/ghosterm-sessiond")
            process.arguments = ["serve", "--runtime-dir", runtimeRoot.appendingPathComponent(String(contract.prefix(16))).path]
            let log = root.appendingPathComponent("daemon.log")
            XCTAssertTrue(FileManager.default.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600]))
            let output = try FileHandle(forWritingTo: log)
            process.standardOutput = output; process.standardError = output
            try process.run(); daemon = process
            let socket = runtimeRoot.appendingPathComponent(contract.prefix(16) + "/session.sock")
            let deadline = Date().addingTimeInterval(3)
            while !FileManager.default.fileExists(atPath: socket.path), process.isRunning, Date() < deadline { Thread.sleep(forTimeInterval: 0.005) }
            XCTAssertTrue(FileManager.default.fileExists(atPath: socket.path), "owned daemon must actually listen at \(socket.path): \((try? String(contentsOf: log, encoding: .utf8)) ?? "no diagnostic")")
        } catch { XCTFail("owned session fixture failed: \(error)") }
    }
    nonisolated func testCaseDidFinish(_ testCase: XCTestCase) {
        MainActor.assumeIsolated {
            stopRuntime()
            generation = String(UUID().uuidString.prefix(8))
        }
    }
    private func stopRuntime() {
        if let daemon, daemon.isRunning {
            daemon.terminate()
            let deadline = Date().addingTimeInterval(5)
            while daemon.isRunning, Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
            if daemon.isRunning { Darwin.kill(daemon.processIdentifier, SIGKILL) }
            daemon.waitUntilExit()
        }
        daemon = nil
    }
    nonisolated func testBundleDidFinish(_ testBundle: Bundle) {
        MainActor.assumeIsolated {
            stopRuntime()
            try? FileManager.default.removeItem(at: root)
        }
    }
}

@MainActor func isolatedAppModel(configPath: String? = nil, knownHostsPath: String? = nil, settings: SettingsStore? = nil, sessionDirectory: URL? = nil,
    remoteInstallRoot: String? = nil, remoteRuntimeDirectory: String? = nil) -> AppModel {
    let fixture = AppModelFixture.shared
    fixture.ensureRuntime()
    return AppModel(configPath: configPath, knownHostsPath: knownHostsPath, settings: settings,
        sessionDirectory: sessionDirectory ?? fixture.root.appendingPathComponent("records-" + UUID().uuidString),
        sessionRuntimeRoot: fixture.runtimeRoot,
        remoteInstallRoot: remoteInstallRoot, remoteRuntimeDirectory: remoteRuntimeDirectory)
}

@MainActor func sessionTestDirectory() -> URL { AppModelFixture.shared.root.appendingPathComponent("profile-" + UUID().uuidString) }

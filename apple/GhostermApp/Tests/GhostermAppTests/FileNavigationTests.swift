import Foundation
import XCTest
@testable import GhostermAppModel

final class NavigationBackend: FileBackend, @unchecked Sendable {
    let displayName = "Owned navigation fixture"
    let initialPath = "/root"
    private let lock = NSLock()
    private var counts: [String: Int] = [:]
    private var failures: [String: Int] = [:]
    private let listing: (String, Int) throws -> [FileEntry]
    init(listing: @escaping (String, Int) throws -> [FileEntry] = { path, _ in
        [FileEntry(path: path + "/entry", name: "entry", kind: .directory)]
    }) { self.listing = listing }
    func failNext(_ path: String) { lock.lock(); failures[path, default: 0] += 1; lock.unlock() }
    func calls(_ path: String) -> Int { lock.lock(); defer { lock.unlock() }; return counts[path, default: 0] }
    func list(_ path: String) throws -> [FileEntry] {
        lock.lock()
        counts[path, default: 0] += 1
        let ordinal = counts[path]!
        let fail = failures[path, default: 0] > 0
        if fail { failures[path]! -= 1 }
        lock.unlock()
        if fail { throw NSError(domain: "OwnedNavigation", code: 1) }
        return try listing(path, ordinal)
    }
    func mkdir(_ path: String, mode: UInt32) throws { throw unexpectedWrite() }
    func remove(_ path: String, recursive: Bool) throws { throw unexpectedWrite() }
    func rename(_ from: String, _ to: String) throws { throw unexpectedWrite() }
    func chmod(_ path: String, mode: UInt32) throws { throw unexpectedWrite() }
    func realpath(_ path: String) throws -> String { path }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {
        for entry in try list(root) { if !visit(entry) { break } }
    }
    private func unexpectedWrite() -> Error { NSError(domain: "Unexpected fixture write", code: 1) }
}

@MainActor
final class FileNavigationTests: XCTestCase {
    private func until(_ predicate: () -> Bool, timeout: TimeInterval = 3) async throws -> Bool {
        let limit = ProcessInfo.processInfo.systemUptime + timeout
        while !predicate(), ProcessInfo.processInfo.systemUptime < limit { try await Task.sleep(for: .milliseconds(2)) }
        return predicate()
    }
    private func loaded(_ model: FilesPanelModel, _ path: String) async throws -> Bool {
        try await until { model.currentPath == path && !model.isLoading && model.errorText == nil }
    }
    private func mainFence() async {
        await withCheckedContinuation { continuation in DispatchQueue.main.async { continuation.resume() } }
    }

    func testOlderNavigationCannotPublishWhileLatestIsPending() async throws {
        let enteredA = expectation(description: "A reading"), enteredB = expectation(description: "B reading")
        let releaseA = DispatchSemaphore(value: 0), releaseB = DispatchSemaphore(value: 0)
        defer { releaseA.signal(); releaseB.signal() }
        let backend = NavigationBackend { path, _ in
            if path == "/A" { enteredA.fulfill(); _ = releaseA.wait(timeout: .now() + 5) }
            if path == "/B" { enteredB.fulfill(); _ = releaseB.wait(timeout: .now() + 5) }
            return [FileEntry(path: path + "/entry", name: path, kind: .file)]
        }
        let model = FilesPanelModel(backend: backend)
        model.navigate(to: "/root", record: false)
        guard try await loaded(model, "/root") else { return XCTFail("initial fixture read did not settle") }
        model.navigate(to: "/A")
        await fulfillment(of: [enteredA], timeout: 3)
        model.navigate(to: "/B"); releaseA.signal()
        await fulfillment(of: [enteredB], timeout: 3)
        await mainFence()
        let preserved = model.currentPath == "/root" && model.entries.first?.name == "/root" && model.isLoading
        releaseB.signal()
        let latest = try await loaded(model, "/B")
        XCTAssertTrue(preserved && latest, "an old response must not flash its path/entries or finish the latest spinner")
    }

    func testSupersededQueuedNavigationDoesNotStartAnotherBackendRead() async throws {
        let entered = expectation(description: "blocking read")
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let backend = NavigationBackend { path, _ in
            if path == "/A" { entered.fulfill(); _ = release.wait(timeout: .now() + 5) }
            return []
        }
        let model = FilesPanelModel(backend: backend)
        model.navigate(to: "/A")
        await fulfillment(of: [entered], timeout: 3)
        model.navigate(to: "/B"); model.navigate(to: "/C"); release.signal()
        let latest = try await loaded(model, "/C")
        XCTAssertTrue(latest && backend.calls("/B") == 0 && backend.calls("/C") == 1)
    }

    func testFailedBackKeepsHistoryForRetry() async throws {
        let backend = NavigationBackend()
        let panel = FilesPanelModel(backend: backend)
        panel.navigate(to: "/A", record: false)
        guard try await loaded(panel, "/A") else { return XCTFail("initial A") }
        panel.navigate(to: "/B")
        guard try await loaded(panel, "/B") else { return XCTFail("initial B") }
        panel.selection = ["/B/entry"]
        backend.failNext("/A"); panel.goBack()
        let failed = try await until { !panel.isLoading && panel.errorText != nil }
        let preserved = failed && panel.currentPath == "/B" && panel.canGoBack && !panel.canGoForward &&
            panel.entries.first?.path == "/B/entry" && panel.selection == ["/B/entry"]
        panel.goBack()
        let retried = try await loaded(panel, "/A")
        XCTAssertTrue(preserved && retried && !panel.canGoBack && panel.canGoForward)
    }

    func testFailedForwardKeepsHistoryForRetry() async throws {
        let backend = NavigationBackend()
        let model = FilesPanelModel(backend: backend)
        model.navigate(to: "/A", record: false)
        guard try await loaded(model, "/A") else { return XCTFail("initial A") }
        model.navigate(to: "/B")
        guard try await loaded(model, "/B") else { return XCTFail("initial B") }
        model.goBack()
        guard try await loaded(model, "/A") else { return XCTFail("initial back") }
        backend.failNext("/B"); model.goForward()
        let failed = try await until { !model.isLoading && model.errorText != nil }
        let preserved = failed && model.currentPath == "/A" && !model.canGoBack && model.canGoForward
        model.goForward()
        let retried = try await loaded(model, "/B")
        XCTAssertTrue(preserved && retried && model.canGoBack && !model.canGoForward)
    }

    func testTreeReadErrorIsNotCachedAsEmptyAndCanRetry() async throws {
        let backend = NavigationBackend(); backend.failNext("/root")
        let model = FilesPanelModel(backend: backend)
        model.loadTreeChildren("/root")
        let failed = try await until { model.treeErrors["/root"] != nil && !model.treeLoading.contains("/root") }
        let uncached = failed && model.treeChildren["/root"] == nil
        model.retryTree("/root")
        let retried = try await until { model.treeChildren["/root"]?.count == 1 && !model.treeLoading.contains("/root") }
        XCTAssertTrue(uncached && retried && model.treeErrors["/root"] == nil && backend.calls("/root") == 2)
    }

    func testRefreshInvalidatesActualLocalTreeCache() async throws {
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("tree-refresh-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("first"), withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root.path))
        model.viewMode = .tree; model.loadTreeChildren(root.path)
        guard try await until({ model.treeChildren[root.path]?.count == 1 }) else { return XCTFail("initial actual tree read") }
        try FileManager.default.removeItem(at: root.appendingPathComponent("first"))
        try FileManager.default.createDirectory(at: root.appendingPathComponent("fresh"), withIntermediateDirectories: true)
        model.refresh()
        let refreshed = try await until { !model.isLoading && !model.treeLoading.contains(root.path) }
        XCTAssertTrue(refreshed && model.treeChildrenVisible(root.path).map(\.name) == ["fresh"])
    }

    func testOldTreeCompletionCannotRepopulateRefreshedCache() async throws {
        let oldEntered = expectation(description: "old tree read"), newEntered = expectation(description: "new tree read")
        let releaseOld = DispatchSemaphore(value: 0), releaseNew = DispatchSemaphore(value: 0)
        defer { releaseOld.signal(); releaseNew.signal() }
        let backend = NavigationBackend { path, count in
            if count == 1 { oldEntered.fulfill(); _ = releaseOld.wait(timeout: .now() + 5) }
            if count == 3 { newEntered.fulfill(); _ = releaseNew.wait(timeout: .now() + 5) }
            return [FileEntry(path: path + "/\(count == 1 ? "old" : "fresh")", name: count == 1 ? "old" : "fresh", kind: .directory)]
        }
        let model = FilesPanelModel(backend: backend)
        model.loadTreeChildren("/root")
        await fulfillment(of: [oldEntered], timeout: 3)
        model.refresh(); releaseOld.signal()
        await fulfillment(of: [newEntered], timeout: 3)
        await mainFence()
        let preserved = model.treeChildren["/root"] == nil && model.treeLoading.contains("/root")
        releaseNew.signal()
        let refreshed = try await until { model.treeChildrenVisible("/root").first?.name == "fresh" }
        XCTAssertTrue(preserved && refreshed)
    }
    func testOlderFailureCannotReplaceLatestPendingState() async throws {
        let enteredA = expectation(description: "failed old read"), enteredB = expectation(description: "pending current read")
        let releaseA = DispatchSemaphore(value: 0), releaseB = DispatchSemaphore(value: 0)
        defer { releaseA.signal(); releaseB.signal() }
        let backend = NavigationBackend { path, _ in
            if path == "/A" {
                enteredA.fulfill(); _ = releaseA.wait(timeout: .now() + 5)
                throw NSError(domain: "Old owned failure", code: 1)
            }
            if path == "/B" { enteredB.fulfill(); _ = releaseB.wait(timeout: .now() + 5) }
            return []
        }
        let model = FilesPanelModel(backend: backend)
        model.navigate(to: "/A")
        await fulfillment(of: [enteredA], timeout: 3)
        model.navigate(to: "/B"); releaseA.signal()
        await fulfillment(of: [enteredB], timeout: 3)
        await mainFence()
        let preserved = model.errorText == nil && model.currentPath == "/root" && model.isLoading
        releaseB.signal()
        let finished = try await loaded(model, "/B")
        XCTAssertTrue(preserved && finished)
    }

    func testNavigationLeavesOldSearchResultsBehind() async throws {
        let model = FilesPanelModel(backend: NavigationBackend())
        model.search("entry")
        model.navigate(to: "/A")
        let finished = try await loaded(model, "/A")
        XCTAssertTrue(finished && model.searchMode == .none && model.searchResults.isEmpty && model.scanned == 0)
    }

    func testRepeatedFollowDoesNotSupersedeTheSamePendingDirectory() async throws {
        let entered = expectation(description: "follow read entered")
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let backend = NavigationBackend { _, count in
            if count == 1 { entered.fulfill(); _ = release.wait(timeout: .now() + 5) }
            return []
        }
        let model = FilesPanelModel(backend: backend)
        model.cwdProvider = { "/A" }; model.followsTerminalCwd = true
        model.followTick()
        await fulfillment(of: [entered], timeout: 3)
        model.followTick(); release.signal()
        let finished = try await loaded(model, "/A")
        XCTAssertTrue(finished && backend.calls("/A") == 1, "follow polling must not continually cancel its own pending directory read")
    }

}

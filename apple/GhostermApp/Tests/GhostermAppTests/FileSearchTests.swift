import Foundation
import GhostermCore
@testable import GhostermAppModel
import XCTest

final class SearchBackend: TransferBackend {
    let displayName = "owned search fixture", initialPath = "/root"
    private let lock = NSLock()
    private var resolutions = 0
    var resolutionCount: Int { lock.lock(); defer { lock.unlock() }; return resolutions }
    var action: (String, Int, @escaping () -> Bool, @escaping (FileEntry) -> Bool, @escaping (Int) -> Void) throws -> Bool = { _, _, _, _, _ in true }
    var listAction: (String) throws -> [FileEntry] = { _ in [] }
    func list(_ path: String) throws -> [FileEntry] { try listAction(path) }
    func mkdir(_ path: String, mode: UInt32) throws {}
    func remove(_ path: String, recursive: Bool) throws {}
    func rename(_ from: String, _ to: String) throws {}
    func chmod(_ path: String, mode: UInt32) throws {}
    func realpath(_ path: String) throws -> String {
        lock.lock(); resolutions += 1; lock.unlock(); return path
    }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {}
    func download(remote: String, local: String) throws -> UInt64 { 1 }
    func upload(local: String, remote: String) throws -> UInt64 { 1 }
    func exists(_ path: String) -> Bool { false }
    func pause(jobID: UInt64, _ paused: Bool) throws {}
    func cancel(jobID: UInt64) throws {}
    func search(root: String, pattern: String, maxDepth: Int, isCancelled: @escaping () -> Bool,
                onHit: @escaping (FileEntry) -> Bool, onScanned: @escaping (Int) -> Void) throws -> Bool {
        try action(pattern, maxDepth, isCancelled, onHit, onScanned)
    }
}

@MainActor
final class FileSearchTests: XCTestCase {
    private func finished(_ model: FilesPanelModel) async throws -> Bool {
        for _ in 0..<200 {
            if model.searchMode != .running { return true }
            try await Task.sleep(for: .milliseconds(10))
        }
        return false
    }
    func testBackendFailureKeepsPartialResultsAndAllowsFreshRetry() async throws {
        let backend = SearchBackend()
        backend.action = { _, depth, _, hit, scanned in
            guard depth == 64 else { throw FileSearchError.incomplete("unbounded") }
            scanned(7); _ = hit(FileEntry(path: "/root/a", name: "a", kind: .file))
            throw FileSearchError.incomplete("owned traversal failure")
        }
        let model = FilesPanelModel(backend: backend)
        model.search("a")
        let failed = try await finished(model)
        let preserved = failed && model.searchMode == .failed(hits: 1) && model.scanned == 7 &&
            model.searchResults.map(\.path) == ["/root/a"] && model.errorText?.contains("owned traversal failure") == true
        backend.action = { _, _, _, _, scanned in scanned(2); return false }
        model.search("fresh")
        let retried = try await finished(model)
        XCTAssertTrue(preserved && retried && model.searchMode == .done(usedExec: false, hits: 0) && model.errorText == nil && model.searchResults.isEmpty && model.scanned == 2)
    }
    func testCancelReachesBackendAndDiscardsLateResultsAndFailure() async throws {
        let entered = expectation(description: "search entered"), stopped = expectation(description: "cooperative stop")
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let backend = SearchBackend()
        var observedCancellation = false
        backend.action = { _, _, cancelled, hit, scanned in
            entered.fulfill(); _ = release.wait(timeout: .now() + 5)
            observedCancellation = cancelled(); stopped.fulfill()
            _ = hit(FileEntry(path: "/root/late", name: "late", kind: .file)); scanned(40)
            throw FileSearchError.incomplete("late failure")
        }
        let model = FilesPanelModel(backend: backend)
        model.search("late")
        await fulfillment(of: [entered], timeout: 3)
        model.cancelSearch(); release.signal()
        await fulfillment(of: [stopped], timeout: 3)
        // Drain the queued completion before asserting its inability to republish.
        backend.action = { _, _, _, _, _ in false }
        model.search("next")
        let completed = try await finished(model)
        XCTAssertTrue(observedCancellation && completed && model.searchMode == .done(usedExec: false, hits: 0) && model.searchResults.isEmpty && model.scanned == 0 && model.errorText == nil)
    }
    func testQueuedSearchIsSkippedAndOlderCompletionCannotPublish() async throws {
        let enteredA = expectation(description: "old search"), enteredC = expectation(description: "latest search")
        let releaseA = DispatchSemaphore(value: 0), releaseC = DispatchSemaphore(value: 0)
        defer { releaseA.signal(); releaseC.signal() }
        let backend = SearchBackend(); var calledB = false
        backend.action = { pattern, _, _, hit, scanned in
            if pattern == "A" {
                enteredA.fulfill(); _ = releaseA.wait(timeout: .now() + 5)
                _ = hit(FileEntry(path: "/root/old", name: "old", kind: .file)); scanned(42)
                throw FileSearchError.incomplete("obsolete failure")
            }
            if pattern == "B" { calledB = true }
            if pattern == "C" { enteredC.fulfill(); _ = releaseC.wait(timeout: .now() + 5) }
            return false
        }
        let model = FilesPanelModel(backend: backend)
        model.search("A"); await fulfillment(of: [enteredA], timeout: 3)
        model.search("B"); model.search("C"); releaseA.signal()
        await fulfillment(of: [enteredC], timeout: 3)
        await withCheckedContinuation { continuation in DispatchQueue.main.async { continuation.resume() } }
        let pending = model.searchMode == .running && model.searchResults.isEmpty && model.errorText == nil && model.scanned == 0 && !calledB && backend.resolutionCount == 2
        releaseC.signal(); let completed = try await finished(model)
        XCTAssertTrue(pending && completed && model.searchMode == .done(usedExec: false, hits: 0))
    }
    func testCompletedNavigationCancelsSearchStartedDuringItsPendingRead() async throws {
        let entered = expectation(description: "navigation pending")
        let release = DispatchSemaphore(value: 0); defer { release.signal() }
        let backend = SearchBackend()
        backend.listAction = { _ in entered.fulfill(); _ = release.wait(timeout: .now() + 5); return [] }
        backend.action = { _, _, _, hit, _ in
            _ = hit(FileEntry(path: "/root/old", name: "old", kind: .file)); return true
        }
        let model = FilesPanelModel(backend: backend)
        model.navigate(to: "/new-root"); await fulfillment(of: [entered], timeout: 3)
        model.search("old"); release.signal()
        for _ in 0..<200 {
            if !model.isLoading && model.currentPath == "/new-root" && model.searchMode != .running { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertTrue(!model.isLoading && model.currentPath == "/new-root" && model.searchMode == .none && model.searchResults.isEmpty && model.errorText == nil)
    }
    func testResultBudgetStopsBackendAndCannotBeReportedAsDone() async throws {
        let backend = SearchBackend()
        var stopped = false
        backend.action = { _, _, cancelled, hit, _ in
            for i in 0..<10 {
                if !hit(FileEntry(path: "/root/\(i)", name: "\(i)", kind: .file)) { stopped = cancelled(); break }
            }
            return true
        }
        let model = FilesPanelModel(backend: backend); model.searchLimits.results = 2
        model.search("*")
        let completed = try await finished(model)
        XCTAssertTrue(completed && stopped && model.searchMode == .failed(hits: 2) && model.errorText?.contains("上限") == true)
    }
    func testScanBudgetStopsEvenWithoutAnyMatches() async throws {
        let backend = SearchBackend()
        backend.action = { _, _, cancelled, _, scanned in
            for i in 1...20 { scanned(i); if cancelled() { break } }
            return false
        }
        let model = FilesPanelModel(backend: backend); model.searchLimits.scanned = 3
        model.search("absent")
        let completed = try await finished(model)
        XCTAssertTrue(completed && model.scanned == 4 && model.searchMode == .failed(hits: 0) && model.errorText?.contains("扫描上限") == true)
    }
    func testInvalidInputDoesNotReachBackend() async throws {
        let backend = SearchBackend(); var called = false
        backend.action = { _, _, _, _, _ in called = true; return true }
        let model = FilesPanelModel(backend: backend)
        model.search("bad\0pattern")
        let invalidNul = model.searchMode == .failed(hits: 0)
        model.search("x", maxDepth: 65)
        XCTAssertTrue(invalidNul && model.searchMode == .failed(hits: 0) && model.errorText != nil && !called)
    }
    func testLocalSearchHonorsDepthAndDoesNotFollowDirectorySymlink() async throws {
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("search-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root.appendingPathComponent("d1/d2"), withIntermediateDirectories: true)
        try Data().write(to: root.appendingPathComponent("top"))
        try Data().write(to: root.appendingPathComponent("d1/deep"))
        try FileManager.default.createSymbolicLink(atPath: root.appendingPathComponent("link").path, withDestinationPath: root.appendingPathComponent("d1").path)
        defer { try? FileManager.default.removeItem(at: root) }
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root.path))
        model.search("*", maxDepth: 1)
        let completed = try await finished(model)
        XCTAssertTrue(completed && model.searchMode == .done(usedExec: false, hits: 3) && Set(model.searchResults.map(\.name)) == ["d1", "link", "top"] && model.searchDepth == 1)
    }
    func testMissingLocalRootIsFailureRatherThanEmptySuccess() async throws {
        let root = NSTemporaryDirectory() + "/absent-search-" + UUID().uuidString
        let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        model.search("*")
        let completed = try await finished(model)
        XCTAssertTrue(completed && model.searchMode == .failed(hits: 0) && model.errorText != nil)
    }
    func testMetadataBudgetAndByteIdentityAndThrottling() {
        var limits = FileSearchLimits(); limits.metadataBytes = 50
        let state = FileSearchState(limits: limits)
        let composed = FileEntry(path: "/é", name: "é", kind: .file)
        let decomposed = FileEntry(path: "/e\u{301}", name: "e\u{301}", kind: .file)
        let first = state.append(composed), second = state.append(decomposed), duplicate = state.append(composed)
        let progress = state.progress()
        var extra = 0
        for _ in 0..<99 { _ = state.scan(); if state.progress() != nil { extra += 1 } }
        let accepted = state.append(FileEntry(path: "/" + String(repeating: "x", count: 40), name: "overflow", kind: .file))
        XCTAssertTrue(first && second && duplicate && progress?.entries.count == 2 && extra == 0 && !accepted && state.snapshot().stopped != nil)
    }
}

final class RemoteFindReaderTests: XCTestCase {
    private func read(_ chunks: [SSHExec.ReadResult], status: Int32? = 0,
                      cancelled: @escaping () -> Bool = { false }, lookup: ((String) throws -> FileEntry)? = nil,
                      onHit: ((FileEntry) -> Bool)? = nil, clock: @escaping () -> TimeInterval = { 0 }, limit: Int = 65536, recordLimit: Int? = nil) throws -> [String] {
        var index = 0, found: [String] = []
        try RemoteFindReader.run(root: "/root", pattern: "*", isCancelled: cancelled, read: {
            defer { index += 1 }; return index < chunks.count ? chunks[index] : .eof
        }, exitStatus: { status }, lookup: lookup ?? { FileEntry(path: $0, name: ($0 as NSString).lastPathComponent, kind: .file) }, onHit: {
            found.append($0.path); return onHit?($0) ?? true
        }, onScanned: { _ in }, clock: clock, outputLimit: limit, recordLimit: recordLimit ?? min(65536, limit))
        return found
    }
    func testSplitNulRecordsKeepLiteralUnicodePaths() throws {
        let paths = try read([.data(Data("/root/a\0/root/é".utf8), isStderr: false), .timeout,
                              .data(Data("\0".utf8), isStderr: false), .eof])
        XCTAssertEqual(paths, ["/root/a", "/root/é"])
    }
    func testNonzeroAndUnknownExitAreNotSuccessfulEmptySearches() {
        var failures = 0
        for status: Int32? in [1, nil] { do { _ = try read([.eof], status: status) } catch { failures += 1 } }
        XCTAssertEqual(failures, 2)
    }
    func testStderrIsNotDiscardedAsSuccess() {
        XCTAssertThrowsError(try read([.data(Data("permission denied".utf8), isStderr: true), .eof]))
    }
    func testTruncatedAndInvalidUtf8AndOutsideRootAreRejected() {
        let records = [Data("/root/tail".utf8), Data([47,114,111,111,116,47,255,0]), Data("/outside/a\0".utf8), Data("/root/../outside\0".utf8)]
        var failures = 0
        for record in records { do { _ = try read([.data(record, isStderr: false), .eof]) } catch { failures += 1 } }
        XCTAssertEqual(failures, records.count)
    }
    func testMetadataFailureIsNotSkipped() {
        XCTAssertThrowsError(try read([.data(Data("/root/a\0".utf8), isStderr: false)], lookup: { _ in throw POSIXError(.EACCES) }))
    }
    func testCancelledTimeoutCannotSpinOrLookUpLateMetadata() {
        var checks = 0, lookups = 0
        let cancelled = { checks += 1; return checks >= 3 }
        var rejected = false
        do { _ = try read([.timeout, .data(Data("/root/late\0".utf8), isStderr: false)], cancelled: cancelled, lookup: { path in lookups += 1; return FileEntry(path: path, name: "late", kind: .file) }) } catch { rejected = true }
        XCTAssertTrue(rejected && lookups == 0 && checks == 3)
    }
    func testOutputAndRecordBudgetRejectUnboundedServerStream() {
        var failures = 0
        for stderr in [false, true] {
            do { _ = try read([.data(Data(repeating: 65, count: 40), isStderr: stderr)], limit: 32) }
            catch { if String(describing: error).contains("输出超过上限") { failures += 1 } }
        }
        XCTAssertEqual(failures, 2)
    }
    func testRecordBudgetRejectsPathBeforeEOF() {
        var message = ""
        do { _ = try read([.data(Data(repeating: 65, count: 40), isStderr: false)], limit: 100, recordLimit: 32) }
        catch { message = String(describing: error) }
        XCTAssertTrue(message.contains("路径超过上限"))
    }
    func testDeadlineAndVisitorStopCannotBecomeDone() {
        var now: TimeInterval = -1, failures = 0
        do { _ = try read([.timeout], clock: { now += 31; return now }) } catch { failures += 1 }
        do { _ = try read([.data(Data("/root/a\0".utf8), isStderr: false)], onHit: { _ in false }) } catch { failures += 1 }
        XCTAssertEqual(failures, 2)
    }
    func testOnlyExplicitCapabilityRefusalUsesWalkFallback() throws {
        var fallbacks = 0, failures = 0
        let unsupported = try SFTPFileBackend.runSearch(open: { () throws -> Int in throw GhostermError.status(SSHExec.unsupportedRaw, "owned refusal") }, read: { _ in }, fallback: { fallbacks += 1 }, isCancelled: { false })
        for stage in 0..<2 {
            do { _ = try SFTPFileBackend.runSearch(open: { if stage == 0 { throw GhostermError.failed("network") }; return 1 }, read: { _ in throw GhostermError.failed("read") }, fallback: { fallbacks += 1 }, isCancelled: { false }) } catch { failures += 1 }
        }
        XCTAssertTrue(!unsupported && fallbacks == 1 && failures == 2)
    }
    func testGlobDoesNotExpandLiteralBracketsAndHandlesAdversarialStars() {
        let positive = SFTPFileBackend.matches("[a].txt", "[a].txt") && SFTPFileBackend.matches("ABC", "a?*")
        let negative = SFTPFileBackend.matches(String(repeating: "a", count: 1000), String(repeating: "*a", count: 100) + "b")
        XCTAssertTrue(positive && !negative && !SFTPFileBackend.matches("a.txt", "[a].txt"))
    }
}

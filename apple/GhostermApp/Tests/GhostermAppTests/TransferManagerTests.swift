// TransferManagerTests — 全局传输队列的语义（无网可跑，用假后端）。
//
// 覆盖：入队 → 进度/速度 → 终态；暂停/继续/取消的路由；冲突 skip/rename/
// 批次内冲突决策；重试；目录展开成文件级 job；进度消息解析。
import GhostermCore
@testable import GhostermAppModel
import XCTest

/// 假后端：记录调用、给出 job id、不碰网络。walk 返回一棵固定的树。
final class FakeTransferBackend: TransferBackend {
    var displayName = "fake"
    var initialPath = "/"
    var nextJob: UInt64 = 1
    var moved: [(String, String)] = []
    var paused: [(UInt64, Bool)] = []
    var cancelled: [UInt64] = []
    var existingRemote: Set<String> = []
    var existenceError: Error?
    var mkdirError: Error?
    var controlError: Error?
    /// walk 要产出的条目（相对 root 的完整路径）。
    var walkEntries: [FileEntry] = []
    var submittedDownloads: [(String, String)] = []
    var boundedDownloadLimits: [UInt64] = []
    var overwriteDecisions: [Bool] = []
    var submittedUploads: [(String, String)] = []
    var stagedUploadOverwrite: [Bool] = []
    var walkStarted: DispatchSemaphore?
    var walkGate: DispatchSemaphore?

    func list(_ path: String) throws -> [FileEntry] { walkEntries }
    func mkdir(_ path: String, mode: UInt32) throws {
        if let mkdirError { throw mkdirError }
        existingRemote.insert(path)
    }
    func remove(_ path: String, recursive: Bool) throws {}
    func rename(_ from: String, _ to: String) throws { moved.append((from, to)) }
    func chmod(_ path: String, mode: UInt32) throws {}
    func realpath(_ path: String) throws -> String { path }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {
        walkStarted?.signal()
        _ = walkGate?.wait(timeout: .now() + 5)
        for e in walkEntries { if !visit(e) { break } }
    }
    func exists(_ path: String) -> Bool { existingRemote.contains(path) }
    func checkedExists(_ path: String) throws -> Bool {
        if let existenceError { throw existenceError }
        return exists(path)
    }
    func download(remote: String, local: String) throws -> UInt64 {
        submittedDownloads.append((remote, local)); defer { nextJob += 1 }; return nextJob
    }
    func downloadStaged(remote: String, local: String, overwrite: Bool,
                        maxBytes: UInt64) throws -> UInt64 {
        submittedDownloads.append((remote, local)); boundedDownloadLimits.append(maxBytes)
        overwriteDecisions.append(overwrite)
        defer { nextJob += 1 }; return nextJob
    }
    func upload(local: String, remote: String) throws -> UInt64 {
        submittedUploads.append((local, remote)); defer { nextJob += 1 }; return nextJob
    }
    func uploadStaged(local: String, remote: String, overwrite: Bool) throws -> UInt64 {
        submittedUploads.append((local, remote)); stagedUploadOverwrite.append(overwrite)
        defer { nextJob += 1 }; return nextJob
    }
    func pause(jobID: UInt64, _ p: Bool) throws {
        if let controlError { throw controlError }
        paused.append((jobID, p))
    }
    func cancel(jobID: UInt64) throws {
        if let controlError { throw controlError }
        cancelled.append(jobID)
    }
    func search(root: String, pattern: String, maxDepth: Int,
                isCancelled: @escaping () -> Bool,
                onHit: @escaping (FileEntry) -> Bool,
                onScanned: @escaping (Int) -> Void) throws -> Bool { true }
}

@MainActor
final class TransferManagerTests: XCTestCase {
    private func makeManager() -> TransferManager {
        TransferManager(pump: CoreEventPump())
    }

    private func progress(_ id: UInt64, _ done: UInt64, _ total: UInt64) -> CoreEvent {
        CoreEvent(kind: .progress, sessionID: 0, jobID: id, code: 0,
                  message: "sftp \(done)/\(total)")
    }
    private func jobDone(_ id: UInt64, _ code: Int32, _ msg: String = "") -> CoreEvent {
        CoreEvent(kind: .jobDone, sessionID: 0, jobID: id, code: code, message: msg)
    }

    private func waitForJob(_ manager: TransferManager, id: UUID,
                            _ predicate: (TransferManager.Job) -> Bool,
                            timeout: TimeInterval = 5) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if let job = manager.jobs.first(where: { $0.id == id }), predicate(job) { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        return manager.jobs.first(where: { $0.id == id }).map(predicate) ?? false
    }

    func testParseProgress() {
        XCTAssertEqual(TransferManager.parseProgress("sftp 12/40")?.0, 12)
        XCTAssertEqual(TransferManager.parseProgress("sftp 12/40")?.1, 40)
        XCTAssertNil(TransferManager.parseProgress("garbage"))
    }

    func testSingleDownloadLifecycle() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/a.bin", local: workspaceTestTmp + "gt-a.bin", name: "a.bin",
                          isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        XCTAssertEqual(m.jobs.count, 1)
        XCTAssertEqual(m.jobs[0].status, .running, "单文件应立刻起 job")
        let core = m.jobs[0].coreJob
        XCTAssertEqual(core, 1)

        // 进度 → done/total。
        m.handle(progress(core, 1000, 4000))
        XCTAssertEqual(m.jobs[0].done, 1000)
        XCTAssertEqual(m.jobs[0].total, 4000)

        // 完成 → done。
        m.handle(jobDone(core, 0 /* GT_JOB_OK */))
        XCTAssertEqual(m.jobs[0].status, .done)
    }

    func testDownloadByteLimitReachesBackend() {
        let manager = makeManager(), backend = FakeTransferBackend()
        manager.enqueueDownload(backend, remote: "/r/preview", local: workspaceTestTmp + "preview",
                                name: "preview", isDirectory: false, maxBytes: 32 * 1024 * 1024)
        let id = manager.jobs[0].id
        XCTAssertTrue(waitForJob(manager, id: id) { $0.coreJob == 1 })
        XCTAssertEqual(backend.boundedDownloadLimits, [32 * 1024 * 1024])
        XCTAssertEqual(backend.overwriteDecisions, [false])
        XCTAssertTrue(backend.submittedDownloads.contains(where: { $0.0 == "/r/preview" }))
    }

    func testExplicitOverwriteIsPassedToAtomicDownloadPublisher() throws {
        let dir = workspaceTestTmp + "gt-overwrite-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let destination = dir + "/exists"
        try Data([1]).write(to: URL(fileURLWithPath: destination))
        let backend = FakeTransferBackend(), manager = makeManager()
        manager.conflictResolver = { _, _ in .init(.overwrite) }
        manager.enqueueDownload(backend, remote: "/r/exists", local: destination,
                                name: "exists", isDirectory: false)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs[0].id) { $0.coreJob == 1 })
        XCTAssertEqual(backend.overwriteDecisions, [true])
    }

    func testFailureNotFoldedIntoSuccess() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/x", local: workspaceTestTmp + "gt-x", name: "x", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        let core = m.jobs[0].coreJob
        m.handle(jobDone(core, 1 /* GT_JOB_FAILED */, "boom"))
        if case .failed(let msg) = m.jobs[0].status { XCTAssertEqual(msg, "boom") }
        else { XCTFail("失败必须落到 .failed（不折叠成 done）") }
        m.handle(jobDone(core, 2 /* GT_JOB_CANCELLED */))
        // 已终态；再收一条不崩即可。
        XCTAssertNotEqual(m.jobs[0].status, .done)
    }

    func testPauseResumeCancelRouting() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/x", local: workspaceTestTmp + "gt-x2", name: "x", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        let job = m.jobs[0]
        m.pause(job)
        XCTAssertEqual(m.jobs[0].status, .paused)
        XCTAssertEqual(b.paused.last?.0, job.coreJob)
        XCTAssertEqual(b.paused.last?.1, true)
        m.resume(job)
        XCTAssertEqual(m.jobs[0].status, .running)
        XCTAssertEqual(b.paused.last?.1, false)
        m.cancel(job)
        XCTAssertEqual(m.jobs[0].status, .cancelling)
        XCTAssertEqual(b.cancelled.last, job.coreJob)
        m.handle(jobDone(job.coreJob, 2))
        XCTAssertEqual(m.jobs[0].status, .cancelled)
    }

    func testControlFailureKeepsActiveOwnerAndShowsError() {
        let backend = FakeTransferBackend(), manager = makeManager()
        manager.enqueueDownload(backend, remote: "/r/control", local: workspaceTestTmp + "control",
                                name: "control", isDirectory: false)
        let id = manager.jobs[0].id
        XCTAssertTrue(waitForJob(manager, id: id) { $0.status == .running })
        backend.controlError = NSError(domain: "test", code: 1)
        manager.pause(manager.jobs[0])
        XCTAssertEqual(manager.jobs[0].status, .running)
        XCTAssertNotNil(manager.jobs[0].controlError)
        manager.cancel(manager.jobs[0])
        XCTAssertEqual(manager.jobs[0].status, .running)
        XCTAssertTrue(backend.cancelled.isEmpty)
    }

    func testUnknownTerminalIsNotFailureOrRetryable() {
        let backend = FakeTransferBackend(), manager = makeManager()
        manager.enqueueDownload(backend, remote: "/r/unknown", local: workspaceTestTmp + "unknown",
                                name: "unknown", isDirectory: false)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs[0].id) { $0.status == .running })
        let job = manager.jobs[0]
        manager.handle(jobDone(job.coreJob, 77, "future result"))
        if case .unknown(let detail) = manager.jobs[0].status { XCTAssertEqual(detail, "future result") }
        else { XCTFail("未知结果必须保留为单独状态") }
        XCTAssertFalse(manager.jobs[0].canRetry)
    }

    func testRemoteExistenceFailureStopsBeforeSubmitting() {
        let backend = FakeTransferBackend(), manager = makeManager()
        backend.existenceError = NSError(domain: "test", code: 2)
        manager.enqueueUpload(backend, local: "/local/source", remote: "/remote/source",
                              name: "source", isDirectory: false)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs[0].id) {
            if case .failed = $0.status { return true }; return false
        })
        XCTAssertTrue(backend.submittedUploads.isEmpty)
    }

    func testUploadPassesAtomicPublicationConflictChoice() {
        let backend = FakeTransferBackend(), manager = makeManager()
        backend.existingRemote.insert("/remote/source")
        manager.conflictResolver = { _, _ in .init(.overwrite) }
        manager.enqueueUpload(backend, local: "/local/source", remote: "/remote/source",
                              name: "source", isDirectory: false)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs[0].id) { $0.status == .running })
        XCTAssertEqual(backend.stagedUploadOverwrite, [true])
    }

    func testDirectoryUploadReportsUnknownDirectoryCreation() {
        let backend = FakeTransferBackend(), manager = makeManager()
        backend.mkdirError = NSError(domain: "test", code: 3)
        manager.enqueueUpload(backend, local: "/local/missing", remote: "/remote/new",
                              name: "new", isDirectory: true)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs[0].id) { $0.status.isTerminal })
        guard case .unknown(let detail)? = manager.jobs.first?.status else {
            return XCTFail("mkdir 回包不明必须保留为未知结果")
        }
        XCTAssertTrue(detail.contains("目录创建结果未知 1 项"))
        XCTAssertTrue(backend.submittedUploads.isEmpty)
    }

    func testConflictSkipAndApplyToAll() throws {
        let dir = workspaceTestTmp + "gt-conflict-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let dest = dir + "/exists.bin"
        try Data([1]).write(to: URL(fileURLWithPath: dest))

        let b = FakeTransferBackend()
        let m = makeManager()
        var asked = 0
        m.conflictResolver = { _, _ in asked += 1; return .init(.skip, applyToBatch: true) }
        m.enqueueDownload(b, remote: "/r/e", local: dest, name: "exists.bin", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .skipped })
        XCTAssertEqual(m.jobs.first?.status, .skipped, "选择 skip → 该 job 被跳过")
        XCTAssertTrue(b.submittedDownloads.isEmpty, "skip 不应提交下载")
        XCTAssertEqual(asked, 1)

        // 新任务开始新批次，不继承此前的冲突选择。
        m.enqueueDownload(b, remote: "/r/e2", local: dest, name: "exists.bin", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs.last!.id) { $0.status == .skipped })
        XCTAssertEqual(asked, 2, "新批次必须重新询问")
        XCTAssertEqual(m.jobs.last?.status, .skipped)
    }

    func testConflictRenameCreatesUniqueName() throws {
        let dir = workspaceTestTmp + "gt-rename-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let dest = dir + "/a.txt"
        try Data([1]).write(to: URL(fileURLWithPath: dest))

        let b = FakeTransferBackend()
        let m = makeManager()
        m.conflictResolver = { _, _ in .init(.rename) }
        m.enqueueDownload(b, remote: "/r/a.txt", local: dest, name: "a.txt", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        XCTAssertEqual(m.jobs.first?.name, "a (1).txt", "重命名应给出唯一名")
        XCTAssertEqual(b.submittedDownloads.last?.1, dir + "/a (1).txt")
    }

    func testApplyToAllIsLimitedToOneDirectoryBatch() throws {
        let local = workspaceTestTmp + "batch-conflicts-\(UUID())"
        try FileManager.default.createDirectory(atPath: local, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: local) }
        try Data([1]).write(to: URL(fileURLWithPath: local + "/a"))
        try Data([2]).write(to: URL(fileURLWithPath: local + "/b"))
        let backend = FakeTransferBackend()
        backend.walkEntries = [
            FileEntry(path: "/r/d/a", name: "a", size: 1, kind: .file),
            FileEntry(path: "/r/d/b", name: "b", size: 1, kind: .file),
        ]
        let manager = makeManager()
        var asked = 0
        manager.conflictResolver = { _, _ in asked += 1; return .init(.skip, applyToBatch: true) }
        manager.enqueueDownload(backend, remote: "/r/d", local: local, name: "d", isDirectory: true)
        let firstParent = manager.jobs[0].id
        XCTAssertTrue(waitForJob(manager, id: firstParent) { $0.status.isTerminal })
        XCTAssertEqual(asked, 1, "同一批次后续冲突使用本次选择")
        XCTAssertTrue(backend.submittedDownloads.isEmpty)

        manager.enqueueDownload(backend, remote: "/r/d", local: local, name: "d", isDirectory: true)
        let secondParent = manager.jobs.last!.id
        XCTAssertTrue(waitForJob(manager, id: secondParent) { $0.status.isTerminal })
        XCTAssertEqual(asked, 2, "新批次必须重新询问")
    }

    func testDirectoryDownloadExpands() {
        let b = FakeTransferBackend()
        b.walkEntries = [
            FileEntry(path: "/r/dir", name: "dir", kind: .directory),
            FileEntry(path: "/r/dir/f1", name: "f1", size: 10, kind: .file),
            FileEntry(path: "/r/dir/f2", name: "f2", size: 20, kind: .file),
            FileEntry(path: "/r/dir/link", name: "link", kind: .symlink),
        ]
        let m = makeManager()
        let dir = workspaceTestTmp + "gt-expand-\(UUID().uuidString)"
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        m.enqueueDownload(b, remote: "/r/dir", local: dir, name: "dir", isDirectory: true)

        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && (b.submittedDownloads.count < 2 ||
              m.jobs.filter({ $0.parentID != nil }).contains(where: { $0.coreJob == 0 })) {
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertEqual(b.submittedDownloads.count, 2, "目录展开应提交两个文件 job（不含符号链接）")
        XCTAssertEqual(m.jobs.count, 3, "包含目录汇总任务")
        for child in m.jobs.filter({ $0.coreJob != 0 }) { m.handle(jobDone(child.coreJob, 0)) }
        guard case .partial(let summary) = m.jobs.first?.status else {
            return XCTFail("跳过符号链接的目录必须显示部分结果")
        }
        XCTAssertTrue(summary.contains("成功 2"))
        XCTAssertTrue(summary.contains("跳过 1"))
    }

    func testDirectoryPauseDuringWalkDoesNotSubmitChildren() {
        let b = FakeTransferBackend()
        b.walkEntries = [FileEntry(path: "/r/d/f", name: "f", size: 1, kind: .file)]
        let started = DispatchSemaphore(value: 0), gate = DispatchSemaphore(value: 0)
        b.walkStarted = started; b.walkGate = gate
        let manager = makeManager(), local = workspaceTestTmp + "gt-paused-walk-\(UUID().uuidString)"
        addTeardownBlock { try? FileManager.default.removeItem(atPath: local) }
        manager.enqueueDownload(b, remote: "/r/d", local: local, name: "d", isDirectory: true)
        XCTAssertEqual(started.wait(timeout: .now() + 2), .success)
        let parent = manager.jobs[0]
        manager.pause(parent)
        gate.signal()
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertTrue(b.submittedDownloads.isEmpty, "暂停的目录扫描不能提交新文件")
        manager.resume(parent)
        XCTAssertTrue(waitForJob(manager, id: parent.id) { $0.status == .running })
        XCTAssertTrue(waitForJob(manager, id: parent.id) { _ in b.submittedDownloads.count == 1 })
        XCTAssertEqual(b.submittedDownloads.count, 1)
    }

    func testClearingFinishedChildPreservesDirectoryProgress() {
        let b = FakeTransferBackend()
        b.walkEntries = [
            FileEntry(path: "/r/d/a", name: "a", size: 10, kind: .file),
            FileEntry(path: "/r/d/b", name: "b", size: 20, kind: .file),
        ]
        let manager = makeManager(), local = workspaceTestTmp + "gt-ledger-\(UUID().uuidString)"
        addTeardownBlock { try? FileManager.default.removeItem(atPath: local) }
        manager.enqueueDownload(b, remote: "/r/d", local: local, name: "d", isDirectory: true)
        let parentID = manager.jobs[0].id
        XCTAssertTrue(waitForJob(manager, id: parentID) { _ in
            manager.jobs.filter { $0.parentID == parentID && $0.coreJob != 0 }.count == 2
        })
        XCTAssertEqual(manager.activeCount, 1, "父目录与两个子传输是一项批次")
        let children = manager.jobs.filter { $0.parentID == parentID }
        let first = try! XCTUnwrap(children.first { $0.name == "a" })
        let second = try! XCTUnwrap(children.first { $0.name == "b" })
        manager.handle(progress(first.coreJob, 5, 10))
        manager.handle(jobDone(first.coreJob, 0))
        manager.clearFinished()
        let parentAfterClear = manager.jobs.first { $0.id == parentID }
        XCTAssertEqual(parentAfterClear?.done, 10)
        XCTAssertEqual(parentAfterClear?.total, 30)
        manager.handle(progress(second.coreJob, 20, 20))
        XCTAssertEqual(manager.jobs.first { $0.id == parentID }?.done, 30)
        manager.handle(jobDone(second.coreJob, 0))
        XCTAssertEqual(manager.jobs.first { $0.id == parentID }?.status, .done)
    }

    func testDirectoryOutcomeSummaryDoesNotDependOnCallbackOrder() {
        func run(_ order: [Int]) -> TransferManager.Status? {
            let backend = FakeTransferBackend()
            backend.walkEntries = [
                FileEntry(path: "/r/d/a", name: "a", kind: .file),
                FileEntry(path: "/r/d/b", name: "b", kind: .file),
                FileEntry(path: "/r/d/c", name: "c", kind: .file),
            ]
            let manager = makeManager(), local = workspaceTestTmp + "summary-order-\(UUID())"
            defer { try? FileManager.default.removeItem(atPath: local) }
            manager.enqueueDownload(backend, remote: "/r/d", local: local, name: "d", isDirectory: true)
            let parentID = manager.jobs[0].id
            let deadline = Date().addingTimeInterval(5)
            while Date() < deadline && manager.jobs.filter({ $0.parentID == parentID && $0.coreJob != 0 }).count < 3 {
                RunLoop.main.run(until: Date().addingTimeInterval(0.01))
            }
            let children = manager.jobs.filter { $0.parentID == parentID }.sorted { $0.name < $1.name }
            guard children.count == 3 else { return nil }
            for index in order {
                switch index {
                case 0: manager.handle(jobDone(children[index].coreJob, 0))
                case 1: manager.handle(jobDone(children[index].coreJob, 1, "z failure"))
                default: manager.handle(jobDone(children[index].coreJob, 2))
                }
            }
            return manager.jobs.first { $0.id == parentID }?.status
        }
        let forward = run([0, 1, 2]), reverse = run([2, 1, 0])
        XCTAssertEqual(forward, reverse)
        guard case .partial(let summary)? = forward else { return XCTFail("混合目录结果应为部分完成") }
        XCTAssertTrue(summary.contains("成功 1")); XCTAssertTrue(summary.contains("取消 1"))
        XCTAssertTrue(summary.contains("失败 1")); XCTAssertTrue(summary.contains("z failure"))
    }

    func testDirectoryContainingOnlySymlinksIsNotReportedAsCopied() {
        let backend = FakeTransferBackend()
        backend.walkEntries = [FileEntry(path: "/r/d/link", name: "link", kind: .symlink)]
        let manager = makeManager(), local = workspaceTestTmp + "symlink-only-\(UUID())"
        defer { try? FileManager.default.removeItem(atPath: local) }
        manager.enqueueDownload(backend, remote: "/r/d", local: local, name: "d", isDirectory: true)
        let parentID = manager.jobs[0].id
        XCTAssertTrue(waitForJob(manager, id: parentID) { $0.status.isTerminal })
        guard case .partial(let summary)? = manager.jobs.first(where: { $0.id == parentID })?.status else {
            return XCTFail("全符号链接目录不能报告完整复制")
        }
        XCTAssertTrue(summary.contains("跳过符号链接 1 项"))
        XCTAssertTrue(backend.submittedDownloads.isEmpty)
    }

    func testDirectoryRetryOnlyResubmitsUnsuccessfulChildren() {
        let backend = FakeTransferBackend()
        backend.walkEntries = [
            FileEntry(path: "/r/dir/ok", name: "ok", size: 4, kind: .file),
            FileEntry(path: "/r/dir/retry", name: "retry", size: 7, kind: .file),
        ]
        let manager = makeManager(), local = workspaceTestTmp + "retry-dir-\(UUID())"
        addTeardownBlock { try? FileManager.default.removeItem(atPath: local) }
        manager.enqueueDownload(backend, remote: "/r/dir", local: local, name: "dir", isDirectory: true)
        let parentID = manager.jobs[0].id
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && (backend.submittedDownloads.count < 2 ||
              manager.jobs.filter({ $0.parentID == parentID }).contains(where: { $0.coreJob == 0 })) {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        let children = manager.jobs.filter { $0.parentID == parentID }
        XCTAssertEqual(children.count, 2)
        manager.handle(jobDone(children[0].coreJob, 0))
        manager.handle(jobDone(children[1].coreJob, 1, "retry me"))
        if case .partial(let summary) = manager.jobs.first!.status {
            XCTAssertTrue(summary.contains("成功 1")); XCTAssertTrue(summary.contains("失败 1"))
        } else { XCTFail("部分成功应保留部分完成摘要") }
        XCTAssertTrue(manager.jobs[0].canRetry)
        manager.retry(manager.jobs[0])
        let retryParent = try! XCTUnwrap(manager.jobs.last(where: \.isDirectory)).id
        let retryDeadline = Date().addingTimeInterval(5)
        while Date() < retryDeadline && (backend.submittedDownloads.count < 3 ||
              manager.jobs.filter({ $0.parentID == retryParent }).contains(where: { $0.coreJob == 0 })) {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        XCTAssertEqual(backend.submittedDownloads.count, 3, "成功子项不得随目录重试重复传输")
        XCTAssertEqual(manager.jobs.filter { $0.parentID == retryParent }.count, 1)
        XCTAssertEqual(backend.submittedDownloads.last?.0, "/r/dir/retry")
    }

    func testEmptyDirectoryUploadAndCancellation() throws {
        let dir = workspaceTestTmp + "empty-transfer-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let b = FakeTransferBackend(), manager = makeManager()
        var finished: TransferManager.Status?
        manager.enqueueUpload(b, local: dir, remote: "/empty", name: "empty", isDirectory: true,
                              onFinish: { finished = $0 })
        let deadline = Date().addingTimeInterval(5)
        while finished == nil && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
        XCTAssertEqual(finished, .done, "空目录创建成功也必须通知完成")
        XCTAssertEqual(manager.jobs.first?.status, .done)
        manager.enqueueUpload(b, local: dir, remote: "/cancelled", name: "cancelled", isDirectory: true)
        manager.cancel(manager.jobs.last!)
        XCTAssertEqual(manager.jobs.last?.status, .cancelling)
        XCTAssertTrue(waitForJob(manager, id: manager.jobs.last!.id) { $0.status == .cancelled })
        XCTAssertEqual(manager.jobs.last?.status, .cancelled, "迟到的展开回调不得覆盖取消态")
    }

    func testDirectoryFailureCallsCompletionAndDoesNotPretendSuccess() throws {
        let manager = makeManager(), backend = FakeTransferBackend()
        backend.walkEntries = [FileEntry(path: "/outside", name: "outside", kind: .file)]
        let dir = workspaceTestTmp + "bad-directory-\(UUID())"
        defer { try? FileManager.default.removeItem(atPath: dir) }
        var finished: TransferManager.Status?
        manager.enqueueDownload(backend, remote: "/inside", local: dir, name: "inside", isDirectory: true,
                                onFinish: { finished = $0 })
        let deadline = Date().addingTimeInterval(5)
        while finished == nil && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
        guard case .failed = finished else { return XCTFail("目录失败必须明确回报，不能显示空目录完成") }
        XCTAssertTrue(backend.submittedDownloads.isEmpty)
    }

    func testRetryAfterFailure() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/x", local: workspaceTestTmp + "gt-retry", name: "x", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        let core = m.jobs[0].coreJob
        m.handle(jobDone(core, 1 /* GT_JOB_FAILED */, "boom"))
        guard let failed = m.jobs.first else { return XCTFail("应有 job") }
        m.retry(failed)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        XCTAssertEqual(m.jobs[0].status, .running, "重试应重新起 job")
        XCTAssertNotEqual(m.jobs[0].coreJob, 0)
    }

    func testSpeedAndEta() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/big", local: workspaceTestTmp + "gt-big", name: "big", isDirectory: false)
        XCTAssertTrue(waitForJob(m, id: m.jobs[0].id) { $0.status == .running })
        let core = m.jobs[0].coreJob
        m.handle(progress(core, 0, 1_000_000))
        // 等一个采样间隔（>0.2s）再收第二次进度，才能算出速度。
        RunLoop.main.run(until: Date().addingTimeInterval(0.25))
        m.handle(progress(core, 500_000, 1_000_000))
        XCTAssertGreaterThan(m.jobs[0].speed, 0, "速度应为正")
        XCTAssertNotNil(m.jobs[0].eta, "总量已知时应给出 ETA")
    }
}

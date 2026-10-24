// TransferManagerTests — 全局传输队列的语义（无网可跑，用假后端）。
//
// 覆盖：入队 → 进度/速度 → 终态；暂停/继续/取消的路由；冲突 skip/rename/
// 「应用到全部」；重试；目录展开成文件级 job；进度消息解析。
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
    /// walk 要产出的条目（相对 root 的完整路径）。
    var walkEntries: [FileEntry] = []
    var submittedDownloads: [(String, String)] = []
    var submittedUploads: [(String, String)] = []

    func list(_ path: String) throws -> [FileEntry] { walkEntries }
    func mkdir(_ path: String, mode: UInt32) throws {}
    func remove(_ path: String, recursive: Bool) throws {}
    func rename(_ from: String, _ to: String) throws { moved.append((from, to)) }
    func chmod(_ path: String, mode: UInt32) throws {}
    func realpath(_ path: String) throws -> String { path }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {
        for e in walkEntries { if !visit(e) { break } }
    }
    func exists(_ path: String) -> Bool { existingRemote.contains(path) }
    func download(remote: String, local: String) throws -> UInt64 {
        submittedDownloads.append((remote, local)); defer { nextJob += 1 }; return nextJob
    }
    func upload(local: String, remote: String) throws -> UInt64 {
        submittedUploads.append((local, remote)); defer { nextJob += 1 }; return nextJob
    }
    func pause(jobID: UInt64, _ p: Bool) throws { paused.append((jobID, p)) }
    func cancel(jobID: UInt64) throws { cancelled.append(jobID) }
    func search(root: String, pattern: String, maxDepth: Int,
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

    func testFailureNotFoldedIntoSuccess() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/x", local: workspaceTestTmp + "gt-x", name: "x", isDirectory: false)
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
        let job = m.jobs[0]
        m.pause(job)
        XCTAssertEqual(m.jobs[0].status, .paused)
        XCTAssertEqual(b.paused.last?.0, job.coreJob)
        XCTAssertEqual(b.paused.last?.1, true)
        m.resume(job)
        XCTAssertEqual(m.jobs[0].status, .running)
        XCTAssertEqual(b.paused.last?.1, false)
        m.cancel(job)
        XCTAssertEqual(m.jobs[0].status, .cancelled)
        XCTAssertEqual(b.cancelled.last, job.coreJob)
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
        m.conflictResolver = { _, _ in asked += 1; return .skip }
        m.enqueueDownload(b, remote: "/r/e", local: dest, name: "exists.bin", isDirectory: false)
        XCTAssertEqual(m.jobs.first?.status, .skipped, "选择 skip → 该 job 被跳过")
        XCTAssertTrue(b.submittedDownloads.isEmpty, "skip 不应提交下载")
        XCTAssertEqual(asked, 1)

        // 应用到全部：不再问。
        m.applyToAll = .skip
        m.enqueueDownload(b, remote: "/r/e2", local: dest, name: "exists.bin", isDirectory: false)
        XCTAssertEqual(asked, 1, "已记住「应用到全部」就不再问")
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
        m.conflictResolver = { _, _ in .rename }
        m.enqueueDownload(b, remote: "/r/a.txt", local: dest, name: "a.txt", isDirectory: false)
        XCTAssertEqual(m.jobs.first?.name, "a (1).txt", "重命名应给出唯一名")
        XCTAssertEqual(b.submittedDownloads.last?.1, dir + "/a (1).txt")
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
        m.applyToAll = .overwrite
        m.enqueueDownload(b, remote: "/r/dir", local: dir, name: "dir", isDirectory: true)

        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && b.submittedDownloads.count < 2 {
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertEqual(b.submittedDownloads.count, 2, "目录展开应提交两个文件 job（不含符号链接）")
        XCTAssertEqual(m.jobs.count, 3, "包含目录汇总任务")
        for child in m.jobs.filter({ $0.coreJob != 0 }) { m.handle(jobDone(child.coreJob, 0)) }
        XCTAssertEqual(m.jobs.first?.status, .done, "子任务全部成功后目录汇总才成功")
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
        XCTAssertEqual(manager.jobs.last?.status, .cancelled)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
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
        let core = m.jobs[0].coreJob
        m.handle(jobDone(core, 1 /* GT_JOB_FAILED */, "boom"))
        guard let failed = m.jobs.first else { return XCTFail("应有 job") }
        m.retry(failed)
        XCTAssertEqual(m.jobs[0].status, .running, "重试应重新起 job")
        XCTAssertNotEqual(m.jobs[0].coreJob, 0)
    }

    func testSpeedAndEta() {
        let b = FakeTransferBackend()
        let m = makeManager()
        m.enqueueDownload(b, remote: "/r/big", local: workspaceTestTmp + "gt-big", name: "big", isDirectory: false)
        let core = m.jobs[0].coreJob
        m.handle(progress(core, 0, 1_000_000))
        // 等一个采样间隔（>0.2s）再收第二次进度，才能算出速度。
        RunLoop.main.run(until: Date().addingTimeInterval(0.25))
        m.handle(progress(core, 500_000, 1_000_000))
        XCTAssertGreaterThan(m.jobs[0].speed, 0, "速度应为正")
        XCTAssertNotNil(m.jobs[0].eta, "总量已知时应给出 ETA")
    }
}

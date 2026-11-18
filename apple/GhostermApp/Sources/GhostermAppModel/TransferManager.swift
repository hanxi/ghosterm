// TransferManager — 全局传输队列（S4）。
//
// 跨标签/跨窗口聚合所有上传下载：进度、速度/ETA、暂停/继续/取消/重试、
// 冲突「询问 + 应用到全部」。经唯一的 CoreEventPump 收 PROGRESS/JOB_DONE
// （不再自己轮询——见 CoreEventPump）。
//
// 并发 v1 = 1：core 的每会话 I/O worker 本就把 job 串行跑（FIFO），故这里
// 一次提交多个文件级 job 也天然串行；目录传输在展开阶段按序 mkdir 再逐文件
// 提交（见 enqueue*）。
import Darwin
import Foundation
import GhostermCore

@MainActor
public final class TransferManager: ObservableObject {
    public static let shared = TransferManager()

    public enum Direction: String, Equatable, Sendable { case download, upload }

    public enum Status: Equatable {
        case queued, running, paused, cancelling, done, partial(String), failed(String), unknown(String), cancelled, skipped
        public var isTerminal: Bool {
            switch self {
            case .done, .partial, .failed, .unknown, .cancelled, .skipped: return true
            default: return false
            }
        }
    }

    public struct Job: Identifiable {
        public let id = UUID()
        public var name: String
        public var direction: Direction
        public var local: String
        public var remote: String
        var maxDownloadBytes: UInt64? = nil
        var overwriteDestination = false
        public var status: Status = .queued
        public var controlError: String?
        public var done: UInt64 = 0
        public var total: UInt64 = 0
        public var coreJob: UInt64 = 0
        public var speed: Double = 0        // bytes/s
        public var eta: TimeInterval? = nil
        /// 撑住 backend（会话）到 job 结束；终态时释放。
        var backend: TransferBackend?
        var lastSampleAt: Date?
        var lastSampleDone: UInt64 = 0
        /// 结束回调（Quick Look 预览用：下完临时副本再弹预览）。
        var onFinish: ((Status) -> Void)?
        var retryBackend: (() -> TransferBackend?)?
        var retryFinish: ((Status) -> Void)?
        var isDirectory = false
        var parentID: UUID?
        var cancellation: DirectoryCancellation?
        var batchID = UUID()
        public var canRetry: Bool {
            guard status.isTerminal, status != .done, status != .skipped else { return false }
            if case .unknown = status { return false }
            return true
        }
    }

    @Published public private(set) var jobs: [Job] = []

    public enum Conflict: Equatable { case overwrite, skip, rename }
    public struct ConflictDecision {
        public let choice: Conflict?
        public let applyToBatch: Bool
        public init(_ choice: Conflict?, applyToBatch: Bool = false) {
            self.choice = choice
            self.applyToBatch = applyToBatch
        }
    }
    /// 冲突询问（主线程弹框）。返回 nil = 用户取消该 job。
    public var conflictResolver: ((_ name: String, _ direction: Direction) -> ConflictDecision)?
    private var batchDecisions: [UUID: Conflict] = [:]

    private var pumpToken: UUID?
    private let expandQueue = DispatchQueue(label: "dev.ghosterm.transfers")
    /// Progress accounting survives removal of completed child rows from the UI.
    private var directoryProgress: [UUID: [UUID: (done: UInt64, total: UInt64)]] = [:]

    /// 供测试注入（默认走 CoreEventPump.shared）。
    public init(pump: CoreEventPump = .shared) {
        pumpToken = pump.addListener { [weak self] ev in
            MainActor.assumeIsolated { self?.handle(ev) }
        }
    }

    deinit {
        if let t = pumpToken { CoreEventPump.shared.removeListener(t) }
    }

    // MARK: - 查询

    /// Count root requests; directory summary rows and child file rows are one batch.
    public var activeCount: Int { jobs.filter { $0.parentID == nil && !$0.status.isTerminal }.count }
    public var hasActive: Bool { activeCount > 0 }

    /// 指定后端是否还有在途传输（标签页关闭确认按它判）。
    public func hasActive(for backend: TransferBackend) -> Bool {
        let id = ObjectIdentifier(backend as AnyObject)
        return jobs.contains {
            guard let b = $0.backend else { return false }
            return ObjectIdentifier(b as AnyObject) == id && !$0.status.isTerminal
        }
    }

    // MARK: - 入队

    public func enqueueDownload(_ backend: TransferBackend, remote: String, local: String,
                                name: String, isDirectory: Bool,
                                maxBytes: UInt64? = nil,
                                onFinish: ((Status) -> Void)? = nil) {
        if isDirectory { expandDirectory(backend, direction: .download, remote: remote, local: local, name: name, onFinish: onFinish) }
        else { addAndStart(backend, direction: .download, local: local, remote: remote,
                           name: name, size: 0, maxDownloadBytes: maxBytes, onFinish: onFinish) }
    }

    public func enqueueUpload(_ backend: TransferBackend, local: String, remote: String,
                              name: String, isDirectory: Bool,
                              onFinish: ((Status) -> Void)? = nil) {
        if isDirectory { expandDirectory(backend, direction: .upload, remote: remote, local: local, name: name, onFinish: onFinish) }
        else { addAndStart(backend, direction: .upload, local: local, remote: remote,
                           name: name, size: 0, onFinish: onFinish) }
    }

    private func addAndStart(_ backend: TransferBackend, direction: Direction,
                             local: String, remote: String, name: String, size: UInt64,
                             maxDownloadBytes: UInt64? = nil,
                             onFinish: ((Status) -> Void)? = nil, parentID: UUID? = nil) {
        var j = Job(name: name, direction: direction, local: local, remote: remote)
        j.onFinish = onFinish
        j.retryFinish = onFinish
        j.parentID = parentID
        if let parentID, let parent = jobs.first(where: { $0.id == parentID }) {
            j.batchID = parent.batchID
            j.cancellation = parent.cancellation
        }
        j.total = size
        j.maxDownloadBytes = maxDownloadBytes
        j.backend = backend
        j.retryBackend = { [weak backend] in backend }
        jobs.append(j)
        guard let i = jobs.firstIndex(where: { $0.id == j.id }) else { return }
        start(i)
    }

    // MARK: - 冲突 + 起 job

    private func start(_ i: Int) {
        guard jobs.indices.contains(i), let backend = jobs[i].backend else { return }
        let id = jobs[i].id, original = jobs[i]
        expandQueue.async { [weak self, backend] in
            guard let self else { return }
            let active = DispatchQueue.main.sync {
                MainActor.assumeIsolated { self.jobs.first(where: { $0.id == id })?.status.isTerminal == false }
            }
            guard active else { return }
            var job = original
                var overwriteDestination = false
            do {
                try job.cancellation?.checkpoint()
                var destination = job.direction == .download ? job.local : job.remote
                let exists = job.direction == .download
                    ? try Self.localPathExists(destination)
                    : try backend.checkedExists(destination)
                if exists {
                    let decision = DispatchQueue.main.sync {
                        MainActor.assumeIsolated { self.resolveConflict(name: job.name, direction: job.direction, batchID: original.batchID) }
                    }
                    let choice = decision.choice
                    if decision.applyToBatch, let choice {
                        DispatchQueue.main.sync {
                            MainActor.assumeIsolated { self.batchDecisions[original.batchID] = choice }
                        }
                    }
                    switch choice {
                    case .skip, .none:
                        DispatchQueue.main.async {
                            MainActor.assumeIsolated {
                                if let index = self.jobs.firstIndex(where: { $0.id == id }) {
                                    self.finish(index, status: choice == nil ? .cancelled : .skipped)
                                }
                            }
                        }
                        return
                    case .rename:
                        destination = try Self.uniqueName(destination, isCancelled: {
                            DispatchQueue.main.sync {
                                MainActor.assumeIsolated { self.jobs.first(where: { $0.id == id })?.status.isTerminal != false }
                            }
                        }) { path in
                            job.direction == .download ? try Self.localPathExists(path)
                                : try backend.checkedExists(path)
                        }
                        if job.direction == .download { job.local = destination }
                        else { job.remote = destination }
                        job.name = (destination as NSString).lastPathComponent
                    case .overwrite:
                        overwriteDestination = true
                        break
                    }
                }
                job.overwriteDestination = overwriteDestination
                let core: UInt64
                switch job.direction {
                case .download:
                    core = try backend.downloadStaged(remote: job.remote, local: job.local,
                                                      overwrite: job.overwriteDestination,
                                                      maxBytes: job.maxDownloadBytes ?? UInt64.max)
                case .upload:
                    core = try backend.uploadStaged(local: job.local, remote: job.remote,
                                                    overwrite: job.overwriteDestination)
                }
                DispatchQueue.main.sync {
                    MainActor.assumeIsolated {
                        guard let index = self.jobs.firstIndex(where: { $0.id == id }) else {
                            try? backend.cancel(jobID: core); return
                        }
                        guard !self.jobs[index].status.isTerminal else {
                            try? backend.cancel(jobID: core); return
                        }
                        self.jobs[index].local = job.local; self.jobs[index].remote = job.remote
                        self.jobs[index].name = job.name
                        self.jobs[index].coreJob = core; self.jobs[index].status = .running
                        self.jobs[index].lastSampleAt = nil
                        if let parentID = self.jobs[index].parentID,
                           self.jobs.first(where: { $0.id == parentID })?.status == .paused {
                            do {
                                try backend.pause(jobID: core, true)
                                self.jobs[index].status = .paused
                            } catch { self.jobs[index].controlError = "暂停请求失败：\(error)" }
                        }
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        if let index = self.jobs.firstIndex(where: { $0.id == id }),
                           !self.jobs[index].status.isTerminal {
                            self.finish(index, status: self.jobs[index].cancellation?.cancelled == true
                                ? .cancelled : .failed("\(error)"))
                        }
                    }
                }
            }
        }
    }

    private func resolveConflict(name: String, direction: Direction, batchID: UUID) -> ConflictDecision {
        if let c = batchDecisions[batchID] { return ConflictDecision(c) }
        guard let conflictResolver else { return ConflictDecision(.skip) }
        return conflictResolver(name, direction)
    }

    nonisolated private static func uniqueName(_ path: String, isCancelled: () -> Bool,
                                              exists: (String) throws -> Bool) throws -> String {
        let ns = path as NSString
        let dir = ns.deletingLastPathComponent
        let base = ns.lastPathComponent as NSString
        let ext = base.pathExtension
        let stem = base.deletingPathExtension
        for n in 1...10_000 {
            guard !isCancelled() else { throw GhostermError.status(8, "重命名查找已取消") }
            let cand = ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)"
            let full = (dir as NSString).appendingPathComponent(cand)
            guard full.utf8.count <= 4096 else { throw GhostermError.failed("候选文件名超过安全长度") }
            if try !exists(full) { return full }
        }
        throw GhostermError.failed("可用重命名编号已耗尽")
    }

    nonisolated private static func localPathExists(_ path: String) throws -> Bool {
        var value = stat()
        if lstat(path, &value) == 0 { return true }
        if errno == ENOENT { return false }
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }

    // MARK: - 目录展开（远端 walk / 本地枚举）

    private func expandDirectory(_ backend: TransferBackend, direction: Direction,
                                 remote: String, local: String, name: String,
                                 onFinish: ((Status) -> Void)?) {
        var parent = Job(name: name, direction: direction, local: local, remote: remote)
        parent.batchID = UUID()
        parent.backend = backend; parent.onFinish = onFinish; parent.retryFinish = onFinish
        parent.isDirectory = true
        let cancellation = DirectoryCancellation()
        parent.cancellation = cancellation
        parent.retryBackend = { [weak backend] in backend }
        jobs.append(parent)
        let parentID = parent.id
        expandQueue.async { [weak self] in
            var files: [(String, String, UInt64)] = []
            var skippedLinks = 0
            var createdDirectories: [String] = []
            var uncertainDirectories: [String] = []
            do {
                let fm = FileManager.default
                try cancellation.checkpoint()
                if direction == .download {
                    try cancellation.checkpoint()
                    if try !Self.localPathExists(local) {
                        uncertainDirectories.append(local)
                        try fm.createDirectory(atPath: local, withIntermediateDirectories: true)
                        createdDirectories.append(local)
                        uncertainDirectories.removeAll { $0 == local }
                    }
                    var directories: [String] = [], invalid = false
                    try backend.walk(root: remote, maxDepth: -1,
                                     isCancelled: { cancellation.cancelled }) { e in
                        if cancellation.waitWhilePaused() { return false }
                        if cancellation.cancelled { return false }
                        if e.path == remote && e.isDirectory { return true }
                        let prefix = remote.hasSuffix("/") ? remote : remote + "/"
                        guard e.path.hasPrefix(prefix) else { invalid = true; return false }
                        let rel = String(e.path.dropFirst(prefix.count))
                        guard !rel.split(separator: "/").contains("..") else { invalid = true; return false }
                        let l = (local as NSString).appendingPathComponent(rel)
                        if e.isDirectory { directories.append(l) }
                        else if e.isSymlink { skippedLinks += 1 }
                        else { files.append((l, e.path, e.size)) }
                        return true
                    }
                    try cancellation.checkpoint()
                    if invalid { throw GhostermError.failed("远端返回了目录范围外的路径") }
                    for path in directories {
                        try cancellation.checkpoint()
                        if try !Self.localPathExists(path) {
                            uncertainDirectories.append(path)
                            try fm.createDirectory(atPath: path, withIntermediateDirectories: true)
                            createdDirectories.append(path)
                            uncertainDirectories.removeAll { $0 == path }
                        }
                    }
                } else {
                    try cancellation.checkpoint()
                    if try !backend.checkedExists(remote) {
                        try cancellation.checkpoint()
                        uncertainDirectories.append(remote)
                        try backend.mkdir(remote, mode: 0o755)
                        createdDirectories.append(remote)
                        uncertainDirectories.removeAll { $0 == remote }
                    }
                    var enumerationError: Error?
                    let sourceRoot = URL(fileURLWithPath: local).resolvingSymlinksInPath()
                    guard let enumerator = fm.enumerator(at: sourceRoot,
                        includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey],
                        errorHandler: { _, error in enumerationError = error; return false }) else { throw GhostermError.failed("无法枚举目录：\(local)") }
                    for case let url as URL in enumerator {
                        try cancellation.checkpoint()
                        let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey])
                        if values.isSymbolicLink == true { skippedLinks += 1; enumerator.skipDescendants(); continue }
                        let prefix = sourceRoot.path.hasSuffix("/") ? sourceRoot.path : sourceRoot.path + "/"
                        guard url.path.hasPrefix(prefix) else { throw GhostermError.failed("本地枚举越过源目录") }
                        let rel = String(url.path.dropFirst(prefix.count))
                        let r = (remote as NSString).appendingPathComponent(rel)
                        if values.isDirectory == true {
                            if try !backend.checkedExists(r) {
                                try cancellation.checkpoint()
                                uncertainDirectories.append(r)
                                try backend.mkdir(r, mode: 0o755)
                                createdDirectories.append(r)
                                uncertainDirectories.removeAll { $0 == r }
                            }
                        } else { files.append((url.path, r, UInt64(values.fileSize ?? 0))) }
                    }
                    if let enumerationError { throw enumerationError }
                }
                DispatchQueue.main.async {
                    guard let self, let i = self.jobs.firstIndex(where: { $0.id == parentID }),
                          !self.jobs[i].status.isTerminal else { return }
                    if self.jobs[i].status != .paused { self.jobs[i].status = .running }
                    self.jobs[i].total = files.reduce(0) { $0 + $1.2 }
                    if files.isEmpty {
                        self.finish(i, status: skippedLinks == 0 ? .done
                            : .partial("目录未复制文件：跳过符号链接 \(skippedLinks) 项"))
                        return
                    }
                    let batch = DirectoryCompletion(count: files.count + skippedLinks)
                    for _ in 0..<skippedLinks { batch.record(.skipped) }
                    self.directoryProgress[parentID] = [:]
                    let retainedDirectorySummary = createdDirectories.isEmpty ? "" :
                        "；已创建并保留目录 \(createdDirectories.count) 项"
                    let uncertainDirectorySummary = uncertainDirectories.isEmpty ? "" :
                        "；目录创建结果未知 \(uncertainDirectories.count) 项，请检查目标"
                    for f in files {
                        self.addAndStart(backend, direction: direction, local: f.0, remote: f.1,
                            name: (f.0 as NSString).lastPathComponent, size: f.2, onFinish: { [weak self] status in
                                batch.record(status)
                                if batch.remaining == 0, let self,
                                   let index = self.jobs.firstIndex(where: { $0.id == parentID }),
                                   !self.jobs[index].status.isTerminal {
                                    let summary = batch.summary
                                    if !uncertainDirectorySummary.isEmpty {
                                        self.finish(index, status: .unknown("\(summary)：\(uncertainDirectorySummary)"))
                                    } else if !createdDirectories.isEmpty, summary != .done {
                                        self.finish(index, status: .partial("\(summary)：\(retainedDirectorySummary)"))
                                    } else {
                                        self.finish(index, status: summary)
                                    }
                                }
                            }, parentID: parentID)
                        if let child = self.jobs.last {
                            self.directoryProgress[parentID, default: [:]][child.id] = (0, child.total)
                        }
                        if self.jobs[i].status == .paused, let child = self.jobs.last { self.pause(child) }
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    guard let self, let i = self.jobs.firstIndex(where: { $0.id == parentID }) else { return }
                    let retained = createdDirectories.isEmpty ? "" : "；已创建并保留目录 \(createdDirectories.count) 项"
                    let uncertain = uncertainDirectories.isEmpty ? "" : "；目录创建结果未知 \(uncertainDirectories.count) 项，请检查目标"
                    let status: Status = !uncertainDirectories.isEmpty
                        ? .unknown("目录传输结果未知：\(error)\(retained)\(uncertain)")
                        : !createdDirectories.isEmpty
                            ? .partial("目录传输未完成：\(error)\(retained)")
                            : cancellation.cancelled ? .cancelled : .failed("目录传输失败：\(error)")
                    self.finish(i, status: status)
                }
            }
        }
    }

    final class DirectoryCancellation: @unchecked Sendable {
        private let lock = NSLock()
        private var stopped = false
        private var paused = false
        var cancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
        func cancel() { lock.lock(); stopped = true; paused = false; lock.unlock() }
        func setPaused(_ value: Bool) { lock.lock(); paused = value && !stopped; lock.unlock() }
        func waitWhilePaused() -> Bool {
            while true {
                lock.lock(); let stop = stopped, wait = paused; lock.unlock()
                if stop { return true }
                if !wait { return false }
                Thread.sleep(forTimeInterval: 0.02)
            }
        }
        func checkpoint() throws {
            if waitWhilePaused() { throw GhostermError.failed("目录传输已取消") }
        }
    }

    private final class DirectoryCompletion {
        var remaining: Int
        private(set) var done = 0
        private(set) var skipped = 0
        private(set) var cancelled = 0
        private(set) var failures: [String] = []
        private(set) var unknown: [String] = []
        init(count: Int) { remaining = count }
        func record(_ status: Status) {
            remaining -= 1
            switch status {
            case .done: done += 1
            case .skipped: skipped += 1
            case .cancelled: cancelled += 1
            case .failed(let message): failures.append(message)
            case .unknown(let message): unknown.append(message)
            default: failures.append("子任务未到达确定终态")
            }
        }
        var summary: Status {
            if !unknown.isEmpty {
                return .unknown("目录结果未知：成功 \(done)，未知 \(unknown.count)；请检查目标后再决定下一步。")
            }
            if failures.isEmpty && skipped == 0 && cancelled == 0 { return .done }
            if done == 0 && failures.isEmpty && skipped == 0 { return .cancelled }
            if done == 0 && failures.isEmpty && cancelled == 0 { return .skipped }
            let details = failures.sorted().prefix(3).joined(separator: "；")
            let message = "目录结果：成功 \(done)，跳过 \(skipped)，取消 \(cancelled)，失败 \(failures.count)\(details.isEmpty ? "" : "；\(details)")"
            return done > 0 || (skipped > 0 && cancelled > 0) ? .partial(message) : .failed(message)
        }
    }

    private func finish(_ index: Int, status: Status) {
        guard jobs.indices.contains(index) else { return }
        jobs[index].status = status
        if status == .done { jobs[index].done = jobs[index].total }
        jobs[index].backend = nil
        if jobs[index].parentID == nil {
            batchDecisions[jobs[index].batchID] = nil
            directoryProgress[jobs[index].id] = nil
        }
        jobs[index].speed = 0; jobs[index].eta = nil
        let callback = jobs[index].onFinish
        jobs[index].onFinish = nil
        callback?(status)
    }

    // MARK: - 控制

    public func pause(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        if jobs[i].isDirectory {
            jobs[i].cancellation?.setPaused(true)
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { pause(child) }
            if !jobs[i].status.isTerminal { jobs[i].status = .paused }
            return
        }
        guard jobs[i].coreJob != 0 else { return }
        do {
            try jobs[i].backend?.pause(jobID: jobs[i].coreJob, true)
            jobs[i].controlError = nil
            if jobs[i].status == .running { jobs[i].status = .paused }
        } catch { jobs[i].controlError = "暂停请求失败：\(error)" }
    }

    public func resume(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        if jobs[i].isDirectory {
            jobs[i].cancellation?.setPaused(false)
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { resume(child) }
            if jobs[i].status == .paused { jobs[i].status = .running }
            return
        }
        guard jobs[i].coreJob != 0 else { return }
        do {
            try jobs[i].backend?.pause(jobID: jobs[i].coreJob, false)
            jobs[i].controlError = nil
            if jobs[i].status == .paused { jobs[i].status = .running }
        } catch { jobs[i].controlError = "继续请求失败：\(error)" }
    }

    public func cancel(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        jobs[i].cancellation?.cancel()
        if jobs[i].isDirectory {
            jobs[i].status = .cancelling
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { cancel(child) }
            return
        } else if jobs[i].coreJob != 0 {
            do {
                try jobs[i].backend?.cancel(jobID: jobs[i].coreJob)
                jobs[i].controlError = nil
                jobs[i].status = .cancelling
            } catch {
                jobs[i].controlError = "取消请求失败：\(error)"
                return
            }
            // Keep the backend until Core confirms a terminal result.
            return
        }
        finish(i, status: .cancelled)
    }

    /// 重试：把这条 job 重置成 queued 再起（沿用同一对路径）。
    public func retry(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        guard jobs[i].status.isTerminal else { return }
        guard let backend = jobs[i].retryBackend?() else {
            jobs[i].status = .failed("原传输端点已关闭，无法重试")
            return
        }
        jobs[i].backend = backend
        if jobs[i].isDirectory {
            let previous = jobs[i]
            if !retryDirectory(previous, backend: backend) {
                expandDirectory(backend, direction: previous.direction, remote: previous.remote,
                                local: previous.local, name: previous.name, onFinish: previous.retryFinish)
            }
            return
        }
        jobs[i].onFinish = jobs[i].retryFinish
        jobs[i].status = .queued
        jobs[i].done = 0
        jobs[i].coreJob = 0
        jobs[i].speed = 0
        jobs[i].eta = nil
        jobs[i].controlError = nil
        start(i)
    }

    @discardableResult
    private func retryDirectory(_ previous: Job, backend: TransferBackend) -> Bool {
        let failedChildren = jobs.filter { child in
            guard child.parentID == previous.id else { return false }
            switch child.status {
            case .failed, .cancelled, .skipped: return true
            default: return false
            }
        }
        guard !failedChildren.isEmpty else { return false }
        var parent = Job(name: previous.name, direction: previous.direction,
                         local: previous.local, remote: previous.remote)
        parent.batchID = UUID()
        parent.backend = backend; parent.retryBackend = { [weak backend] in backend }
        parent.onFinish = previous.retryFinish; parent.retryFinish = previous.retryFinish
        parent.isDirectory = true; parent.total = failedChildren.reduce(0) { $0 + $1.total }
        let cancellation = DirectoryCancellation(); parent.cancellation = cancellation
        jobs.append(parent)
        let parentID = parent.id
        directoryProgress[parentID] = [:]
        let batch = DirectoryCompletion(count: failedChildren.count)
        for child in failedChildren {
            addAndStart(backend, direction: child.direction, local: child.local, remote: child.remote,
                name: child.name, size: child.total, onFinish: { [weak self] status in
                    batch.record(status)
                    guard batch.remaining == 0, let self,
                          let index = self.jobs.firstIndex(where: { $0.id == parentID }),
                          !self.jobs[index].status.isTerminal else { return }
                    self.finish(index, status: batch.summary)
                }, parentID: parentID)
            if let row = jobs.last {
                directoryProgress[parentID, default: [:]][row.id] = (0, row.total)
            }
        }
        return true
    }

    public func clearFinished() {
        let activeParents = Set(jobs.filter { $0.isDirectory && !$0.status.isTerminal }.map(\.id))
        jobs.removeAll { job in
            guard job.status.isTerminal else { return false }
            guard let parentID = job.parentID else { return true }
            return !activeParents.contains(parentID)
        }
    }

    public func removeAll() {
        let roots = jobs.filter { !$0.status.isTerminal && $0.parentID == nil }
        roots.forEach(cancel)
        clearFinished()
    }


    // MARK: - 事件

    func handle(_ ev: CoreEvent) {
        guard ev.jobID != 0,
              let i = jobs.firstIndex(where: { $0.coreJob == ev.jobID }) else { return }
        guard !jobs[i].status.isTerminal else { return }
        switch ev.kind {
        case .progress:
            if let (done, total) = Self.parseProgress(ev.message) {
                jobs[i].done = done
                if total > 0 { jobs[i].total = total }
                updateSpeed(i)
                if let parentID = jobs[i].parentID { updateDirectoryProgress(jobIndex: i, parentID: parentID) }
            }
        case .jobDone:
            switch ev.jobResult {
            case .ok: jobs[i].status = .done; jobs[i].done = jobs[i].total
            case .failed: jobs[i].status = .failed(ev.message)
            case .cancelled: jobs[i].status = .cancelled
            case .other: jobs[i].status = .unknown(ev.message.isEmpty ? "Core 返回未知终态" : ev.message)
            }
            if let parentID = jobs[i].parentID { updateDirectoryProgress(jobIndex: i, parentID: parentID) }
            finish(i, status: jobs[i].status)
        default:
            break
        }
    }

    private func updateDirectoryProgress(jobIndex: Int, parentID: UUID) {
        guard jobs.indices.contains(jobIndex), let parent = jobs.firstIndex(where: { $0.id == parentID }) else { return }
        var ledger = directoryProgress[parentID, default: [:]]
        let child = jobs[jobIndex]
        ledger[child.id] = (child.done, child.total)
        directoryProgress[parentID] = ledger
        jobs[parent].done = ledger.values.reduce(0) { $0 + $1.done }
        jobs[parent].total = ledger.values.reduce(0) { $0 + $1.total }
    }

    private func updateSpeed(_ i: Int) {
        let now = Date()
        if let last = jobs[i].lastSampleAt {
            let dt = now.timeIntervalSince(last)
            if dt >= 0.2 {
                let db = Double(jobs[i].done &- min(jobs[i].done, jobs[i].lastSampleDone))
                jobs[i].speed = db / dt
                jobs[i].lastSampleAt = now
                jobs[i].lastSampleDone = jobs[i].done
                if jobs[i].speed > 0, jobs[i].total > jobs[i].done {
                    jobs[i].eta = Double(jobs[i].total - jobs[i].done) / jobs[i].speed
                } else { jobs[i].eta = nil }
            }
        } else {
            jobs[i].lastSampleAt = now
            jobs[i].lastSampleDone = jobs[i].done
        }
    }

    /// 解析 core 的 PROGRESS 消息（"sftp <done>/<total>"）。
    static func parseProgress(_ msg: String) -> (UInt64, UInt64)? {
        let parts = msg.split(separator: " ")
        guard parts.count >= 2 else { return nil }
        let nums = parts[1].split(separator: "/")
        guard nums.count == 2, let d = UInt64(nums[0]), let t = UInt64(nums[1]) else { return nil }
        return (d, t)
    }

    /// 重试使用任务自身的弱端点引用，不跨任务借用会话。
}

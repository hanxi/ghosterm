// TransferManager — 全局传输队列（S4）。
//
// 跨标签/跨窗口聚合所有上传下载：进度、速度/ETA、暂停/继续/取消/重试、
// 冲突「询问 + 应用到全部」。经唯一的 CoreEventPump 收 PROGRESS/JOB_DONE
// （不再自己轮询——见 CoreEventPump）。
//
// 并发 v1 = 1：core 的每会话 I/O worker 本就把 job 串行跑（FIFO），故这里
// 一次提交多个文件级 job 也天然串行；目录传输在展开阶段按序 mkdir 再逐文件
// 提交（见 enqueue*）。
import Foundation
import GhostermCore

@MainActor
public final class TransferManager: ObservableObject {
    public static let shared = TransferManager()

    public enum Direction: String, Equatable, Sendable { case download, upload }

    public enum Status: Equatable {
        case queued, running, paused, done, failed(String), cancelled, skipped
        public var isTerminal: Bool {
            switch self {
            case .done, .failed, .cancelled, .skipped: return true
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
        public var status: Status = .queued
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
    }

    @Published public private(set) var jobs: [Job] = []

    public enum Conflict: Equatable { case overwrite, skip, rename }
    /// 冲突询问（主线程弹框）。返回 nil = 用户取消该 job。
    public var conflictResolver: ((_ name: String, _ direction: Direction) -> Conflict?)?
    /// 「应用到全部」记住的选择（nil = 每次都问）。
    public var applyToAll: Conflict?

    private var pumpToken: UUID?
    private let expandQueue = DispatchQueue(label: "dev.ghosterm.transfers")

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

    public var activeCount: Int { jobs.filter { !$0.status.isTerminal }.count }
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
                                onFinish: ((Status) -> Void)? = nil) {
        if isDirectory { expandDirectory(backend, direction: .download, remote: remote, local: local, name: name, onFinish: onFinish) }
        else { addAndStart(backend, direction: .download, local: local, remote: remote,
                           name: name, size: 0, onFinish: onFinish) }
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
                             onFinish: ((Status) -> Void)? = nil, parentID: UUID? = nil) {
        var j = Job(name: name, direction: direction, local: local, remote: remote)
        j.onFinish = onFinish
        j.retryFinish = onFinish
        j.parentID = parentID
        j.total = size
        j.backend = backend
        j.retryBackend = { [weak backend] in backend }
        jobs.append(j)
        guard let i = jobs.firstIndex(where: { $0.id == j.id }) else { return }
        start(i)
    }

    // MARK: - 冲突 + 起 job

    private func start(_ i: Int) {
        var j = jobs[i]
        guard let backend = j.backend else { return }
        // 冲突：落点已存在 → 询问（或沿用「应用到全部」）。
        let dest = j.direction == .download ? j.local : j.remote
        let exists = j.direction == .download
            ? FileManager.default.fileExists(atPath: dest)
            : backend.exists(dest)
        if exists {
            let choice = resolveConflict(name: j.name, direction: j.direction)
            switch choice {
            case .skip, .none:
                finish(i, status: choice == nil ? .cancelled : .skipped)
                return
            case .rename:
                let newDest = Self.uniqueName(dest, isRemote: j.direction == .upload,
                                              exists: { p in
                    j.direction == .download ? FileManager.default.fileExists(atPath: p)
                                             : backend.exists(p)
                })
                if j.direction == .download { jobs[i].local = newDest }
                else { jobs[i].remote = newDest }
                jobs[i].name = (newDest as NSString).lastPathComponent
                j = jobs[i]
            case .overwrite:
                break
            }
        }
        do {
            let core: UInt64
            switch j.direction {
            case .download: core = try backend.download(remote: j.remote, local: j.local)
            case .upload: core = try backend.upload(local: j.local, remote: j.remote)
            }
            jobs[i].coreJob = core
            jobs[i].status = .running
            jobs[i].lastSampleAt = nil
        } catch {
            finish(i, status: .failed("\(error)"))
        }
    }

    private func resolveConflict(name: String, direction: Direction) -> Conflict? {
        if let c = applyToAll { return c }
        guard let conflictResolver else { return .skip }
        return conflictResolver(name, direction)
    }

    private static func uniqueName(_ path: String, isRemote: Bool,
                                   exists: (String) -> Bool) -> String {
        let ns = path as NSString
        let dir = ns.deletingLastPathComponent
        let base = ns.lastPathComponent as NSString
        let ext = base.pathExtension
        let stem = base.deletingPathExtension
        var n = 1
        while true {
            let cand = ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)"
            let full = (dir as NSString).appendingPathComponent(cand)
            if !exists(full) { return full }
            n += 1
        }
    }

    // MARK: - 目录展开（远端 walk / 本地枚举）

    private func expandDirectory(_ backend: TransferBackend, direction: Direction,
                                 remote: String, local: String, name: String,
                                 onFinish: ((Status) -> Void)?) {
        var parent = Job(name: name, direction: direction, local: local, remote: remote)
        parent.backend = backend; parent.onFinish = onFinish; parent.retryFinish = onFinish
        parent.isDirectory = true
        let cancellation = DirectoryCancellation()
        parent.cancellation = cancellation
        parent.retryBackend = { [weak backend] in backend }
        jobs.append(parent)
        let parentID = parent.id
        expandQueue.async { [weak self] in
            var files: [(String, String, UInt64)] = []
            do {
                let fm = FileManager.default
                try cancellation.check()
                if direction == .download {
                    try fm.createDirectory(atPath: local, withIntermediateDirectories: true)
                    var directories: [String] = [], invalid = false
                    try backend.walk(root: remote, maxDepth: -1) { e in
                        if cancellation.cancelled { return false }
                        if e.path == remote && e.isDirectory { return true }
                        let prefix = remote.hasSuffix("/") ? remote : remote + "/"
                        guard e.path.hasPrefix(prefix) else { invalid = true; return false }
                        let rel = String(e.path.dropFirst(prefix.count))
                        guard !rel.split(separator: "/").contains("..") else { invalid = true; return false }
                        let l = (local as NSString).appendingPathComponent(rel)
                        if e.isDirectory { directories.append(l) }
                        else if !e.isSymlink { files.append((l, e.path, e.size)) }
                        return true
                    }
                    try cancellation.check()
                    if invalid { throw GhostermError.failed("远端返回了目录范围外的路径") }
                    for path in directories { try cancellation.check(); try fm.createDirectory(atPath: path, withIntermediateDirectories: true) }
                } else {
                    if !backend.exists(remote) { try backend.mkdir(remote, mode: 0o755) }
                    var enumerationError: Error?
                    let sourceRoot = URL(fileURLWithPath: local).resolvingSymlinksInPath()
                    guard let enumerator = fm.enumerator(at: sourceRoot,
                        includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey],
                        errorHandler: { _, error in enumerationError = error; return false }) else { throw GhostermError.failed("无法枚举目录：\(local)") }
                    for case let url as URL in enumerator {
                        try cancellation.check()
                        let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey])
                        if values.isSymbolicLink == true { enumerator.skipDescendants(); continue }
                        let prefix = sourceRoot.path.hasSuffix("/") ? sourceRoot.path : sourceRoot.path + "/"
                        guard url.path.hasPrefix(prefix) else { throw GhostermError.failed("本地枚举越过源目录") }
                        let rel = String(url.path.dropFirst(prefix.count))
                        let r = (remote as NSString).appendingPathComponent(rel)
                        if values.isDirectory == true {
                            if !backend.exists(r) { try backend.mkdir(r, mode: 0o755) }
                        } else { files.append((url.path, r, UInt64(values.fileSize ?? 0))) }
                    }
                    if let enumerationError { throw enumerationError }
                }
                DispatchQueue.main.async {
                    guard let self, let i = self.jobs.firstIndex(where: { $0.id == parentID }),
                          !self.jobs[i].status.isTerminal else { return }
                    if self.jobs[i].status != .paused { self.jobs[i].status = .running }
                    self.jobs[i].total = files.reduce(0) { $0 + $1.2 }
                    if files.isEmpty { self.finish(i, status: .done); return }
                    let batch = DirectoryCompletion(count: files.count)
                    for f in files {
                        self.addAndStart(backend, direction: direction, local: f.0, remote: f.1,
                            name: (f.0 as NSString).lastPathComponent, size: f.2, onFinish: { [weak self] status in
                                batch.remaining -= 1
                                if case .failed = status { batch.result = status }
                                else if case .failed = batch.result { /* 保留明确失败 */ }
                                else if status != .done { batch.result = status }
                                if batch.remaining == 0, let self,
                                   let index = self.jobs.firstIndex(where: { $0.id == parentID }),
                                   !self.jobs[index].status.isTerminal {
                                    self.finish(index, status: batch.result)
                                }
                            }, parentID: parentID)
                        if self.jobs[i].status == .paused, let child = self.jobs.last { self.pause(child) }
                    }
                }
            } catch {
                DispatchQueue.main.async {
                    guard let self, let i = self.jobs.firstIndex(where: { $0.id == parentID }) else { return }
                    self.finish(i, status: cancellation.cancelled ? .cancelled : .failed("目录传输失败：\(error)"))
                }
            }
        }
    }

    final class DirectoryCancellation: @unchecked Sendable {
        private let lock = NSLock()
        private var stopped = false
        var cancelled: Bool { lock.lock(); defer { lock.unlock() }; return stopped }
        func cancel() { lock.lock(); stopped = true; lock.unlock() }
        func check() throws { if cancelled { throw GhostermError.failed("目录传输已取消") } }
    }

    private final class DirectoryCompletion {
        var remaining: Int
        var result: Status = .done
        init(count: Int) { remaining = count }
    }

    private func finish(_ index: Int, status: Status) {
        guard jobs.indices.contains(index) else { return }
        jobs[index].status = status
        if status == .done { jobs[index].done = jobs[index].total }
        jobs[index].backend = nil
        jobs[index].speed = 0; jobs[index].eta = nil
        let callback = jobs[index].onFinish
        jobs[index].onFinish = nil
        callback?(status)
    }

    // MARK: - 控制

    public func pause(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        if jobs[i].isDirectory {
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { pause(child) }
            if !jobs[i].status.isTerminal { jobs[i].status = .paused }
            return
        }
        guard jobs[i].coreJob != 0 else { return }
        try? jobs[i].backend?.pause(jobID: jobs[i].coreJob, true)
        if jobs[i].status == .running { jobs[i].status = .paused }
    }

    public func resume(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        if jobs[i].isDirectory {
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { resume(child) }
            if jobs[i].status == .paused { jobs[i].status = .running }
            return
        }
        guard jobs[i].coreJob != 0 else { return }
        try? jobs[i].backend?.pause(jobID: jobs[i].coreJob, false)
        if jobs[i].status == .paused { jobs[i].status = .running }
    }

    public func cancel(_ job: Job) {
        guard let i = jobs.firstIndex(where: { $0.id == job.id }) else { return }
        jobs[i].cancellation?.cancel()
        if jobs[i].isDirectory {
            for child in jobs.filter({ $0.parentID == job.id && !$0.status.isTerminal }) { cancel(child) }
        } else if jobs[i].coreJob != 0 { try? jobs[i].backend?.cancel(jobID: jobs[i].coreJob) }
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
            let old = jobs.remove(at: i)
            expandDirectory(backend, direction: old.direction, remote: old.remote, local: old.local,
                            name: old.name, onFinish: old.retryFinish)
            return
        }
        jobs[i].onFinish = jobs[i].retryFinish
        jobs[i].status = .queued
        jobs[i].done = 0
        jobs[i].coreJob = 0
        jobs[i].speed = 0
        jobs[i].eta = nil
        start(i)
    }

    public func clearFinished() {
        jobs.removeAll { $0.status.isTerminal }
    }

    public func removeAll() { jobs.removeAll() }


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
                if let parentID = jobs[i].parentID, let parent = jobs.firstIndex(where: { $0.id == parentID }) {
                    let children = jobs.filter { $0.parentID == parentID }
                    jobs[parent].done = children.reduce(0) { $0 + $1.done }
                    jobs[parent].total = children.reduce(0) { $0 + $1.total }
                }
            }
        case .jobDone:
            switch ev.jobResult {
            case .ok: jobs[i].status = .done; jobs[i].done = jobs[i].total
            case .failed: jobs[i].status = .failed(ev.message)
            case .cancelled: jobs[i].status = .cancelled
            case .other: jobs[i].status = .failed(ev.message)
            }
            finish(i, status: jobs[i].status)
        default:
            break
        }
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

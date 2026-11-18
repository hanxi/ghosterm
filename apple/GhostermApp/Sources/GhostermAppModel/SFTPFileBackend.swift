// SFTPFileBackend — FileBackend 的远端实现（core gt_sftp_*）。
//
// 与 LocalFileBackend 共用同一套 FileBackend 接缝；面板 UI 零分叉。
//
// 线程：本类**自持有串行队列**并把 SFTP 请求放在上面（queue.sync）——
// 浏览/搜索/旧 job 操作由调用方后台 owner 调用；libssh2 始终只在这一条队列上
// 执行。pause/cancel 只更新 Core 的 job 标志，可从控制路径直接调用。SSHSession 由本
// 对象持有（谁建会话谁撑住；释放顺序：先 sftp.close 再 session.close）。
import Foundation
import Darwin
import GhostermCore

public final class SFTPFileBackend: TransferBackend {
    /// 撑住会话生命周期（同 SFTPBrowser 的教训）。
    public let session: SSHSession
    public let sftp: SFTPSession
    public let initialPath: String
    public let displayName: String

    /// 本后端的唯一 libssh2 调用队列（见文件头）。
    private let queue = DispatchQueue(label: "dev.ghosterm.sftp-backend")
    private let jobLock = NSCondition()
    private var activeJobs = Set<UInt64>()
    private var exclusiveReceive = false
    private var eventToken: UUID?

    public init(session: SSHSession, sftp: SFTPSession, user: String, host: String,
                initialPath: String = ".") {
        self.session = session
        self.sftp = sftp
        self.displayName = "\(user)@\(host)"
        self.initialPath = initialPath
        eventToken = CoreEventPump.shared.addListener { [weak self] event in
            guard event.kind == .jobDone else { return }
            guard let self else { return }
            self.jobLock.lock()
            self.activeJobs.remove(event.jobID)
            self.jobLock.broadcast()
            self.jobLock.unlock()
        }
    }

    deinit {
        if let eventToken { CoreEventPump.shared.removeListener(eventToken) }
        // 顺序铁律：先关 SFTP 子系统（shutdown 要用 session），再关 SSH 会话。
        let ownerQueue = queue, sftp = sftp, session = session
        ownerQueue.async {
            sftp.close()
            session.close()
        }
    }

    // MARK: - FileBackend

    public func list(_ path: String) throws -> [FileEntry] {
        try queue.sync {
            try withExclusiveOwner { timeout in
                try sftp.browse(path: path, timeoutMilliseconds: timeout).entries.map(Self.entry(from:))
            }
        }
    }

    public func mkdir(_ path: String, mode: UInt32) throws {
        try queue.sync {
            try withExclusiveOwner { timeout in
                let value = path as NSString
                let parent = value.deletingLastPathComponent
                let name = value.lastPathComponent
                let canonicalParent = try sftp.realpath(parent.isEmpty ? "." : parent)
                let result = try sftp.createDirectory(parent: canonicalParent,
                                                      name: name, timeoutMilliseconds: timeout)
                guard result.state == .complete else {
                    throw GhostermError.status(result.status, "远端目录创建未确认（状态：\(result.state)）")
                }
                if mode != 0o755 {
                    try sftp.chmod(path: canonicalParent + "/" + name, mode: mode)
                }
            }
        }
    }

    public func remove(_ path: String, recursive: Bool) throws {
        try queue.sync {
            try withExclusiveOwner { _ in
                try recursive ? sftp.removeAll(path: path) : sftp.remove(path: path)
            }
        }
    }

    public func rename(_ from: String, _ to: String) throws {
        try queue.sync { try withExclusiveOwner { _ in try sftp.rename(from: from, to: to) } }
    }

    public func chmod(_ path: String, mode: UInt32) throws {
        try queue.sync { try withExclusiveOwner { _ in try sftp.chmod(path: path, mode: mode) } }
    }

    public func realpath(_ path: String) throws -> String {
        try queue.sync { try withExclusiveOwner { _ in try sftp.realpath(path) } }
    }

    public func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {
        try walk(root: root, maxDepth: maxDepth, isCancelled: { false }, visit: visit)
    }

    public func walk(root: String, maxDepth: Int, isCancelled: @escaping () -> Bool,
                     visit: @escaping (FileEntry) -> Bool) throws {
        try queue.sync {
            try withExclusiveOwner(cancelled: isCancelled) { _ in
                do {
                    try sftp.walk(root: root, maxDepth: maxDepth) { e in
                        !isCancelled() && visit(Self.entry(from: e))
                    }
                } catch let e as GhostermError where e.isCancelled {
                    // visitor 请求停止不是错误（协作式取消）。
                }
                if isCancelled() { throw GhostermError.failed("目录遍历已取消") }
            }
        }
    }

    /// 远端路径是否存在（符号链接按**链接自身**判存在，不跟随）。
    public func exists(_ path: String) -> Bool {
        (try? checkedExists(path)) == true
    }

    public func checkedExists(_ path: String) throws -> Bool {
        do { _ = try queue.sync { try withExclusiveOwner { _ in try lstatLocked(path) } }; return true }
        catch let error as GhostermError {
            if case .status(let code, _) = error, code == 2 { return false }
            throw error
        }
    }

    /// 已在队列上时的 lstat（**不可**再 queue.sync——同队列重入会死锁）。
    private func lstatLocked(_ path: String) throws -> SFTPStat {
        try sftp.lstat(path: path)
    }

    // MARK: - 传输（提交/pause/cancel 都不触 libssh2 的用户面，但仍走队列保序）

    public func download(remote: String, local: String) throws -> UInt64 {
        try downloadStaged(remote: remote, local: local, overwrite: true, maxBytes: UInt64.max)
    }
    public func downloadStaged(remote: String, local: String, overwrite: Bool,
                               maxBytes: UInt64) throws -> UInt64 {
        try queue.sync {
            jobLock.lock(); defer { jobLock.unlock() }
            guard !exclusiveReceive else {
                throw GhostermError.failed("文件 owner 正在执行独占受控读取")
            }
            let id = try sftp.downloadStaged(remote: remote, local: local,
                                             overwrite: overwrite, maxBytes: maxBytes)
            activeJobs.insert(id)
            return id
        }
    }
    public func upload(local: String, remote: String) throws -> UInt64 {
        try queue.sync {
            jobLock.lock(); defer { jobLock.unlock() }
            guard !exclusiveReceive else {
                throw GhostermError.failed("文件 owner 正在执行独占受控读取")
            }
            let id = try sftp.upload(local: local, remote: remote)
            activeJobs.insert(id)
            return id
        }
    }

    /// Run a bounded receive on the backend's sole SFTP owner queue. It refuses
    /// to overlap legacy worker jobs and blocks new submissions until the read
    /// and destination close have both completed.
    public func receivePreview(remote: String, local: String, maxBytes: UInt64,
                               cancellation: SSHCancellation,
                               completion: @escaping @MainActor (Result<UInt64, Error>) -> Void) {
        queue.async { [weak self] in
            guard let self else { return }
            let result = Result {
                try self.withExclusiveOwner { timeout in
                    guard !cancellation.isCancelled else { throw GhostermError.failed("预览已取消") }
                    let descriptor = Darwin.open(local, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
                    guard descriptor >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
                    let count: UInt64
                    do {
                        count = try self.sftp.receive(path: remote, destinationDescriptor: descriptor,
                                                      maxBytes: maxBytes, timeoutMilliseconds: timeout,
                                                      cancellation: cancellation)
                    } catch {
                        _ = Darwin.ftruncate(descriptor, 0)
                        _ = Darwin.close(descriptor)
                        _ = Darwin.unlink(local)
                        throw error
                    }
                    guard Darwin.close(descriptor) == 0 else {
                        let error = POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                        _ = Darwin.unlink(local)
                        throw error
                    }
                    return count
                }
            }
            DispatchQueue.main.async { MainActor.assumeIsolated { completion(result) } }
        }
    }

    private func withExclusiveOwner<T>(timeoutMilliseconds: UInt32 = 30_000,
                                       cancelled: () -> Bool = { false },
                                       operation: (UInt32) throws -> T) throws -> T {
        let deadline = Date().addingTimeInterval(Double(timeoutMilliseconds) / 1000)
        jobLock.lock()
        while exclusiveReceive || !activeJobs.isEmpty {
            if cancelled() {
                jobLock.unlock()
                throw GhostermError.failed("等待文件连接时已取消")
            }
            let tick = min(deadline, Date().addingTimeInterval(0.05))
            guard jobLock.wait(until: tick) || Date() < deadline else {
                jobLock.unlock()
                throw GhostermError.failed("等待文件连接空闲超时")
            }
        }
        if cancelled() {
            jobLock.unlock()
            throw GhostermError.failed("文件操作已取消")
        }
        exclusiveReceive = true
        jobLock.unlock()
        defer {
            jobLock.lock(); exclusiveReceive = false; jobLock.broadcast(); jobLock.unlock()
        }
        let remaining = deadline.timeIntervalSinceNow
        guard remaining > 0 else { throw GhostermError.failed("文件操作预算已耗尽") }
        return try operation(UInt32(max(1, min(Double(UInt32.max), remaining * 1000))))
    }
    public func pause(jobID: UInt64, _ paused: Bool) throws {
        // Core serializes these request flags with its job table; they do not
        // touch libssh2 and must remain callable while the IO owner is busy.
        try sftp.pause(jobID: jobID, paused)
    }
    public func cancel(jobID: UInt64) throws {
        try sftp.cancel(jobID: jobID)
    }

    // MARK: - 远端搜索（exec 优先，walk 回退）

    public func search(root: String, pattern: String, maxDepth: Int,
                       isCancelled: @escaping () -> Bool,
                       onHit: @escaping (FileEntry) -> Bool,
                       onScanned: @escaping (Int) -> Void) throws -> Bool {
        guard root.hasPrefix("/"), !root.utf8.contains(0), (0...64).contains(maxDepth) else {
            throw FileSearchError.incomplete("远端搜索目录或深度无效")
        }
        return try queue.sync {
          try withExclusiveOwner(cancelled: isCancelled) { _ in
            try Self.runSearch(open: { try self.session.exec(Self.findCommand(root: root, maxDepth: maxDepth)) },
                read: { exec in
                    defer { exec.close() }
                    try RemoteFindReader.run(root: root, pattern: pattern, isCancelled: isCancelled,
                        read: { try exec.read(max: 32768, timeoutMS: 100) }, exitStatus: { exec.exitStatus() },
                        lookup: { path in Self.entry(path: path, stat: try self.lstatLocked(path)) },
                        onHit: onHit, onScanned: onScanned)
                }, fallback: {
                    var scanned = 0
                    do {
                        try self.sftp.walk(root: root, maxDepth: maxDepth) { entry in
                            if isCancelled() { return false }
                            scanned += 1; onScanned(scanned)
                            if isCancelled() { return false }
                            return !Self.matches(entry.name, pattern) || onHit(Self.entry(from: entry))
                        }
                    } catch let error as GhostermError where error.isCancelled && isCancelled() {
                        throw FileSearchError.cancelled
                    }
                    if isCancelled() { throw FileSearchError.cancelled }
                }, isCancelled: isCancelled)
          }
        }
    }

    /// Only an explicit exec capability refusal permits a walk fallback. Network,
    /// authentication and command/read failures must retain their original error.
    static func runSearch<T>(open: () throws -> T, read: (T) throws -> Void,
                             fallback: () throws -> Void, isCancelled: () -> Bool) throws -> Bool {
        if isCancelled() { throw FileSearchError.cancelled }
        let channel: T
        do { channel = try open() }
        catch let error as GhostermError where error.isExecUnsupported {
            if isCancelled() { throw FileSearchError.cancelled }
            try fallback(); return false
        }
        try read(channel)
        return true
    }

    static func findCommand(root: String, maxDepth: Int) -> String {
        // Filter locally so find's bracket syntax and locale cannot change the
        // name matching contract. -mindepth excludes the root in both engines.
        "find \(Self.sq(root)) -mindepth 1 -maxdepth \(maxDepth) -print0"
    }

    // MARK: - 映射

    private static func entry(from e: SFTPEntry) -> FileEntry {
        FileEntry(path: e.fullPath, name: e.name, size: e.size, mtime: e.mtime,
                  mode: e.mode, kind: kind(dir: e.isDirectory, link: e.isSymlink, regular: e.isRegularFile),
                  uid: e.uid, gid: e.gid, hasOwner: e.hasOwner, sizeKnown: e.hasSize)
    }
    private static func entry(from e: SFTPWalkEntry) -> FileEntry {
        FileEntry(path: e.path, name: e.name, size: e.stat.size, mtime: e.stat.mtime,
                  mode: e.stat.mode,
                  kind: kind(dir: e.stat.isDirectory, link: e.stat.isSymlink, regular: e.stat.isRegularFile),
                  uid: e.stat.uid, gid: e.stat.gid, hasOwner: e.stat.hasOwner, sizeKnown: e.stat.hasSize)
    }
    private static func entry(path: String, stat: SFTPStat) -> FileEntry {
        FileEntry(path: path, name: (path as NSString).lastPathComponent,
                  size: stat.size, mtime: stat.mtime, mode: stat.mode,
                  kind: kind(dir: stat.isDirectory, link: stat.isSymlink, regular: stat.isRegularFile),
                  uid: stat.uid, gid: stat.gid, hasOwner: stat.hasOwner, sizeKnown: stat.hasSize)
    }
    private static func kind(dir: Bool, link: Bool, regular: Bool) -> FileEntry.Kind {
        if link { return .symlink }
        if dir { return .directory }
        return regular ? .file : .other
    }

    // MARK: - 小工具

    private static func sq(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// 简单 glob（* / ?）匹配；无通配符时按「包含子串」不区分大小写。
    public static func matches(_ name: String, _ pattern: String) -> Bool {
        let p = pattern.lowercased()
        let n = name.lowercased()
        if !p.contains("*") && !p.contains("?") { return n.contains(p) }
        let text = Array(n), tokens = Array(p)
        var i = 0, j = 0, star: Int?, retry = 0
        while i < text.count {
            if j < tokens.count && (tokens[j] == "?" || tokens[j] == text[i]) { i += 1; j += 1 }
            else if j < tokens.count && tokens[j] == "*" { star = j; j += 1; retry = i }
            else if let star { retry += 1; i = retry; j = star + 1 }
            else { return false }
        }
        while j < tokens.count && tokens[j] == "*" { j += 1 }
        return j == tokens.count
    }
}

private extension GhostermError {
    var isExecUnsupported: Bool {
        if case .status(let code, _) = self { return code == SSHExec.unsupportedRaw }
        return false
    }
}

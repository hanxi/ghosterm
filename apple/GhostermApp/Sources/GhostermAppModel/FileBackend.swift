// FileBackend — 文件浏览器的后端接缝（Local / SFTP）。
//
// 三层接缝（铁律 #1）的延伸：UI（FilesPanelView）只认 FileBackend，**不关心**
// 背后是本地文件系统还是远端 SFTP——本地实现走 Foundation，远程实现走 core
// 的 gt_sftp_*。两档挂在同一个面板后面，故 UI 与业务代码零分叉。
//
// 线程纪律：实现都是**阻塞**的，由 FilesPanelModel 在它自己的串行队列上
// 驱动；远程实现对底层 SFTPSession 的使用因此天然单线程（libssh2 契约）。
import Darwin
import Foundation
import GhostermCore

/// 文件浏览器里的一条目（本地 / 远程统一模型）。
public struct FileEntry: Identifiable, Equatable {
    public enum Kind: Equatable { case directory, file, symlink, other }

    public let path: String
    public let name: String
    public let size: UInt64
    public let sizeKnown: Bool
    public let mtime: Date?
    public let mode: UInt32
    public let kind: Kind
    public let uid: UInt32
    public let gid: UInt32
    public let hasOwner: Bool

    public init(path: String, name: String, size: UInt64 = 0, mtime: Date? = nil,
                mode: UInt32 = 0, kind: Kind, uid: UInt32 = 0, gid: UInt32 = 0,
                hasOwner: Bool = false, sizeKnown: Bool = true) {
        self.path = path
        self.name = name
        self.size = size
        self.sizeKnown = sizeKnown
        self.mtime = mtime
        self.mode = mode
        self.kind = kind
        self.uid = uid
        self.gid = gid
        self.hasOwner = hasOwner
    }

    public var id: String { path }
    public var isDirectory: Bool { kind == .directory }
    public var isSymlink: Bool { kind == .symlink }
    public var isHidden: Bool { name.hasPrefix(".") }

    public var sizeText: String {
        if isDirectory || !sizeKnown { return "—" }
        guard size <= UInt64(Int64.max) else { return "超出显示范围" }
        return ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file)
    }

    /// 权限位字符串（八进制 3 位）。
    public var modeText: String {
        mode == 0 ? "—" : String(mode & 0o777, radix: 8)
    }
}

/// 后端能力：列目录 + 写侧原语（本地与远程都实现）。
public protocol FileBackend: AnyObject {
    var displayName: String { get }
    var initialPath: String { get }
    func list(_ path: String) throws -> [FileEntry]
    func mkdir(_ path: String, mode: UInt32) throws
    func remove(_ path: String, recursive: Bool) throws
    func rename(_ from: String, _ to: String) throws
    func chmod(_ path: String, mode: UInt32) throws
    func realpath(_ path: String) throws -> String
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws
    func walk(root: String, maxDepth: Int, isCancelled: @escaping () -> Bool,
              visit: @escaping (FileEntry) -> Bool) throws
}

/// 只会远端有的能力：传输 + 远端搜索。
public protocol TransferBackend: FileBackend {
    func download(remote: String, local: String) throws -> UInt64
    func downloadStaged(remote: String, local: String, overwrite: Bool,
                        maxBytes: UInt64) throws -> UInt64
    func upload(local: String, remote: String) throws -> UInt64
    func uploadStaged(local: String, remote: String, overwrite: Bool) throws -> UInt64
    /// 远端路径是否存在（冲突检测用；符号链接按链接自身判）。
    func exists(_ path: String) -> Bool
    /// Errors are distinct from confirmed absence; callers must not use a
    /// failed stat as authorization to create or overwrite a destination.
    func checkedExists(_ path: String) throws -> Bool
    func pause(jobID: UInt64, _ paused: Bool) throws
    func cancel(jobID: UInt64) throws
    /// 递归搜索：优先走远端 exec（find/grep，一次往返）；服务器禁 exec 时
    /// **显式回退** SFTP walk。返回 true 表示用的是 exec 快路径。
    func search(root: String, pattern: String, maxDepth: Int,
                isCancelled: @escaping () -> Bool,
                onHit: @escaping (FileEntry) -> Bool,
                onScanned: @escaping (Int) -> Void) throws -> Bool
}

public extension TransferBackend {
    /// Backends without atomic remote publication preserve their established behavior.
    func uploadStaged(local: String, remote: String, overwrite: Bool) throws -> UInt64 {
        try upload(local: local, remote: remote)
    }
}

public extension TransferBackend {
    func checkedExists(_ path: String) throws -> Bool { exists(path) }
    func downloadStaged(remote: String, local: String, overwrite: Bool,
                        maxBytes: UInt64) throws -> UInt64 {
        _ = overwrite
        throw GhostermError.failed("此传输后端不支持有字节上限的下载")
    }
}

public extension FileBackend {
    func walk(root: String, maxDepth: Int, isCancelled: @escaping () -> Bool,
              visit: @escaping (FileEntry) -> Bool) throws {
        guard !isCancelled() else { throw GhostermError.failed("目录遍历已取消") }
        try walk(root: root, maxDepth: maxDepth) { entry in
            !isCancelled() && visit(entry)
        }
        if isCancelled() { throw GhostermError.failed("目录遍历已取消") }
    }
}

// MARK: - 本地后端（Foundation / POSIX）

public final class LocalFileBackend: FileBackend {
    public var displayName: String { "本地" }
    public let initialPath: String
    private let fm = FileManager.default

    public init(initialPath: String? = nil) {
        let p = initialPath ?? NSHomeDirectory()
        self.initialPath = p.isEmpty ? "/" : p
    }

    public func list(_ path: String) throws -> [FileEntry] {
        let names = try fm.contentsOfDirectory(atPath: path)
        return try names.map { try entry(at: join(path, $0)) }
    }

    public func mkdir(_ path: String, mode: UInt32) throws {
        try fm.createDirectory(atPath: path, withIntermediateDirectories: false,
                               attributes: [.posixPermissions: NSNumber(value: mode)])
    }

    public func remove(_ path: String, recursive: Bool) throws {
        if recursive {
            try fm.removeItem(atPath: path)
            return
        }
        var st = stat()
        guard lstat(path, &st) == 0 else {
            throw GhostermError.failed("lstat 失败：\(path)")
        }
        let isDir = (st.st_mode & S_IFMT) == S_IFDIR
        let rc = isDir ? Darwin.rmdir(path) : Darwin.unlink(path)
        if rc != 0 {
            throw GhostermError.failed("删除失败：\(path)（\(String(cString: strerror(errno))))")
        }
    }

    public func rename(_ from: String, _ to: String) throws {
        try fm.moveItem(atPath: from, toPath: to)
    }

    public func chmod(_ path: String, mode: UInt32) throws {
        if chmod_(path, mode) != 0 {
            throw GhostermError.failed("chmod 失败：\(path)")
        }
    }

    public func realpath(_ path: String) throws -> String {
        (path as NSString).resolvingSymlinksInPath
    }

    public func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {
        let rootURL = URL(fileURLWithPath: root)
        let rootEntry = try entry(at: root)
        guard rootEntry.isDirectory else { throw GhostermError.failed("遍历来源不是目录：\(root)") }
        var enumerationError: Error?
        guard let enumerator = fm.enumerator(at: rootURL, includingPropertiesForKeys: nil, options: [], errorHandler: { _, error in
            enumerationError = error
            return false
        }) else { throw GhostermError.failed("无法枚举：\(root)") }
        while let url = enumerator.nextObject() as? URL {
            let depth = enumerator.level
            if maxDepth >= 0 && depth > maxDepth { enumerator.skipDescendants(); continue }
            let item = try entry(at: url.path)
            if item.isSymlink || (item.isDirectory && maxDepth >= 0 && depth >= maxDepth) {
                enumerator.skipDescendants()
            }
            if !visit(item) { return }
        }
        if let enumerationError { throw enumerationError }
    }

    // MARK: helpers

    private func chmod_(_ path: String, _ mode: UInt32) -> Int32 {
        var before = Darwin.stat()
        guard Darwin.lstat(path, &before) == 0 else { return -1 }
        guard before.st_mode & mode_t(S_IFMT) != mode_t(S_IFLNK) else { errno = ELOOP; return -1 }
        let fd = Darwin.open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { return -1 }
        defer { Darwin.close(fd) }
        var opened = Darwin.stat()
        guard Darwin.fstat(fd, &opened) == 0 else { return -1 }
        guard before.st_dev == opened.st_dev && before.st_ino == opened.st_ino else { errno = EAGAIN; return -1 }
        return Darwin.fchmod(fd, mode_t(mode & 0o7777))
    }

    private func join(_ dir: String, _ name: String) -> String {
        (dir as NSString).appendingPathComponent(name)
    }

    private func entry(at full: String) throws -> FileEntry {
        var st = stat()
        guard lstat(full, &st) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
        let kind: FileEntry.Kind
        switch st.st_mode & S_IFMT {
        case S_IFDIR: kind = .directory
        case S_IFLNK: kind = .symlink
        case S_IFREG: kind = .file
        default: kind = .other
        }
        var mtime: Date?
        if st.st_mtimespec.tv_sec != 0 || st.st_mtimespec.tv_nsec != 0 {
            mtime = Date(timeIntervalSince1970: TimeInterval(st.st_mtimespec.tv_sec))
        }
        return FileEntry(
            path: full,
            name: (full as NSString).lastPathComponent,
            size: kind == .directory ? 0 : UInt64(max(0, st.st_size)),
            mtime: mtime, mode: UInt32(st.st_mode & 0o7777), kind: kind,
            uid: st.st_uid, gid: st.st_gid, hasOwner: true)
    }
}

import Darwin
import Foundation

/// One private install transaction on the destination filesystem. Ownership is
/// established by mkdir + fd identity, never adopted by a filename prefix.
final class PluginReleaseStage {
    enum Failure: String, Error, LocalizedError {
        case root, io, changed, conflict, busy, limit
        var errorDescription: String? {
            switch self {
            case .root: return "安装根目录须为当前用户的私有普通目录。"
            case .io: return "安装文件写入、同步或清理失败。"
            case .changed: return "安装目录身份已变化，保留原位置供检查。"
            case .conflict: return "此发布版本已存在；未覆盖原版本。"
            case .busy: return "已有发布包正在检查或等待安装，请先取消或完成它。"
            case .limit: return "已达到 64 个保留版本的安装上限。"
            }
        }
    }
    struct PublicationFailure: LocalizedError {
        let code: Int32
        var errorDescription: String? { "发布目录重命名失败（errno \(code)）。" }
    }
    let rootURL: URL
    private(set) var name: String
    private var parent: Int32 = -1, root: Int32 = -1
    private var identity = stat(), parentIdentity = stat()
    private var published = false, directories = Set<String>()
    private var removed = false
    private var visited = 0
    private struct FileStamp: Equatable {
        let device: dev_t, inode: ino_t, size: off_t, mode: mode_t, owner: uid_t, links: nlink_t
        let modified: Int, modifiedNS: Int, changed: Int, changedNS: Int
        init(_ value: stat) {
            device = value.st_dev; inode = value.st_ino; size = value.st_size; mode = value.st_mode
            owner = value.st_uid; links = value.st_nlink
            modified = value.st_mtimespec.tv_sec; modifiedNS = value.st_mtimespec.tv_nsec
            changed = value.st_ctimespec.tv_sec; changedNS = value.st_ctimespec.tv_nsec
        }
    }
    private var files: [String: FileStamp] = [:]
    private var verifiedFiles = 0
    private var journal: PluginInstallJournal?
    private var journalRecord: PluginInstallJournal.Record?
    private var journalReference: PluginInstallJournal.Reference?
    private(set) var maintenanceIssue: String?
    private let parentSync: (Int32) -> Int32
    private let checkpoint: (String) throws -> Void
    private let renameExclusive: (Int32, String, String) -> Int32
    var payloadURL: URL { rootURL.appendingPathComponent(name + "/payload", isDirectory: true) }

    init(directory: URL, parentSync: @escaping (Int32) -> Int32 = { fsync($0) }, checkpoint: @escaping (String) throws -> Void = { _ in },
         renameExclusive: @escaping (Int32, String, String) -> Int32 = {
             renameatx_np($0, $1, $0, $2, UInt32(RENAME_EXCL))
         }) throws {
        guard directory.isFileURL, directory.path.hasPrefix("/"), directory.path.utf8.count <= 3500,
              !directory.path.utf8.contains(0) else { throw Failure.root }
        rootURL = directory
        self.parentSync = parentSync
        self.checkpoint = checkpoint
        self.renameExclusive = renameExclusive
        name = ".prepare-" + UUID().uuidString.lowercased()
        // The application owns this dedicated root; only its final component is
        // created here. Do not silently create a chain through an unknown root.
        if mkdir(directory.path, 0o700) != 0 && errno != EEXIST { throw Failure.root }
        parent = open(directory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard parent >= 0 else { throw Failure.root }
        guard fstat(parent, &parentIdentity) == 0, parentIdentity.st_uid == geteuid(),
              parentIdentity.st_mode & 0o077 == 0 else { Darwin.close(parent); parent = -1; throw Failure.root }
        guard flock(parent, LOCK_EX | LOCK_NB) == 0 else { Darwin.close(parent); parent = -1; throw Failure.busy }
        do {
            var pending = stat()
            if fstatat(parent, PluginReleaseCatalog.removalName, &pending, AT_SYMLINK_NOFOLLOW) == 0 { throw Failure.busy }
            guard errno == ENOENT else { throw Failure.io }
            let copy = fcntl(parent, F_DUPFD_CLOEXEC, 3)
            guard copy >= 0 else { throw Failure.io }
            guard let stream = fdopendir(copy) else { Darwin.close(copy); throw Failure.io }
            defer { closedir(stream) }
            var count = 0
            while true {
                errno = 0
                guard let entry = readdir(stream) else { guard errno == 0 else { throw Failure.io }; break }
                let dot = withUnsafePointer(to: &entry.pointee.d_name) {
                    $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) }
                }
                if dot != "." && dot != ".." && dot != PluginReleaseCatalog.selectionName && dot != PluginReleaseCatalog.removalName && dot != PluginInstallJournal.directoryName { count += 1 }
                guard count < 64 else { throw Failure.limit }
            }
        } catch { Darwin.close(parent); parent = -1; throw error }
        do {
            let journal = try PluginInstallJournal(parent: parent, create: true)!
            let id = String(name.dropFirst(".prepare-".count))
            journalRecord = try journal.reserve(id: id)
            journalReference = try journal.read(id).1
            self.journal = journal
            try checkpoint("after-reservation")
        } catch { Darwin.close(parent); parent = -1; throw error }
        guard mkdirat(parent, name, 0o700) == 0 else { Darwin.close(parent); parent = -1; throw Failure.io }
        root = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard root >= 0, fstat(root, &identity) == 0 else {
            if root >= 0 { Darwin.close(root) }; root = -1
            _ = unlinkat(parent, name, AT_REMOVEDIR); Darwin.close(parent); parent = -1
            throw Failure.io
        }
        do {
            guard flock(root, LOCK_EX | LOCK_NB) == 0 else { throw Failure.busy }
            try checkpoint("after-directory-creation")
            var record = journalRecord!
            record.directory = PluginInstallJournal.Identity(identity)
            try journal!.write(record, expected: journalReference!.digest)
            journalRecord = record; journalReference = try journal!.read(record.id).1
            guard fsync(parent) == 0 else { throw Failure.io }
            try checkpoint("after-directory-registration")
        } catch {
            // An interrupted or failed identity update is retained as unknown;
            // the reservation alone must never adopt this directory on restart.
            Darwin.close(root); root = -1; Darwin.close(parent); parent = -1
            throw error
        }
    }
    deinit { if root >= 0 { Darwin.close(root) }; if parent >= 0 { Darwin.close(parent) } }
    func catalogLease() throws -> Int32 {
        try validate()
        let copy = fcntl(parent, F_DUPFD_CLOEXEC, 3)
        guard copy >= 0 else { throw Failure.io }
        return copy
    }

    private func same(_ a: stat, _ b: stat) -> Bool {
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode & S_IFMT == S_IFDIR && a.st_uid == geteuid()
    }
    private func validate() throws {
        var value = stat()
        guard !published, fstatat(parent, name, &value, AT_SYMLINK_NOFOLLOW) == 0,
              same(value, identity), fstat(root, &value) == 0, same(value, identity) else { throw Failure.changed }
        let path = open(rootURL.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard path >= 0 else { throw Failure.changed }
        defer { Darwin.close(path) }
        guard fstat(path, &value) == 0, same(value, parentIdentity), value.st_mode & 0o077 == 0 else { throw Failure.changed }
    }
    func write(_ data: Data, path: String, executable: Bool = false) throws {
        try validate()
        guard PluginReleaseArchive.validPath(path, maxDepth: 34) else { throw PluginReleaseArchive.Failure.path }
        var parts = path.split(separator: "/").map(String.init)
        let leaf = parts.removeLast()
        var fd = fcntl(root, F_DUPFD_CLOEXEC, 3)
        guard fd >= 0 else { throw Failure.io }
        defer { Darwin.close(fd) }
        var prefix = ""
        for part in parts {
            prefix += (prefix.isEmpty ? "" : "/") + part
            if directories.insert(prefix).inserted {
                guard directories.count <= 4096, mkdirat(fd, part, 0o700) == 0 else { throw Failure.io }
            }
            let next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            guard next >= 0 else { throw Failure.io }
            Darwin.close(fd); fd = next
        }
        let output = openat(fd, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard output >= 0 else { throw Failure.io }
        defer { Darwin.close(output) }
        try checkpoint("after-file-create")
        try data.withUnsafeBytes { buffer in
            var at = 0
            while at < buffer.count {
                let count = Darwin.write(output, buffer.baseAddress!.advanced(by: at), buffer.count - at)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw Failure.io }; at += count
            }
        }
        guard fchmod(output, executable ? 0o500 : 0o400) == 0, fsync(output) == 0 else { throw Failure.io }
        var value = stat()
        guard fstat(output, &value) == 0 else { throw Failure.io }
        files[path] = FileStamp(value)
        try checkpoint("after-file-write")
    }

    /// Atomic, non-overwriting publication. A failed parent fsync after rename
    /// is reported separately: publication has happened; durability is unknown.
    func publish(id: String, beforeRename: (() throws -> Void)? = nil) throws -> (directory: URL, durabilityConfirmed: Bool) {
        guard id.utf8.count == 64, id.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { throw Failure.io }
        try validate()
        visited = 0; verifiedFiles = 0
        try walk(root, removing: false)
        guard verifiedFiles == files.count else { throw Failure.changed }
        try validate()
        if var record = journalRecord, let journal, let reference = journalReference {
            record.target = id
            try journal.write(record, expected: reference.digest)
            journalRecord = record; journalReference = try journal.read(record.id).1
        }
        try checkpoint("before-publication-rename")
        try beforeRename?()
        var result = renameExclusive(parent, name, id)
        var code = result == 0 ? 0 : errno
        if code == EACCES {
            // Some macOS volumes require the source directory to be writable.
            // Keep the parent lock held and leave descendant modes untouched.
            try validate()
            guard fchmod(root, 0o700) == 0 else { throw PublicationFailure(code: errno) }
            result = renameExclusive(parent, name, id)
            code = result == 0 ? 0 : errno
            if result == 0 { published = true; name = id }
            guard fchmod(root, 0o500) == 0, fsync(root) == 0 else { throw PublicationFailure(code: errno) }
        }
        if code == EEXIST { throw Failure.conflict }
        guard result == 0 else { throw PublicationFailure(code: code) }
        published = true
        name = id
        try checkpoint("after-publication-rename")
        let durable = parentSync(parent) == 0
        try checkpoint("after-publication-sync")
        if durable, let journal, let reference = journalReference {
            do { try journal.retire(reference) }
            catch { maintenanceIssue = "版本已发布；安装归属记录保留待核对：\(error)" }
        } else if !durable { maintenanceIssue = "发布目录持久性未确认，安装归属记录已保留。" }
        _ = flock(root, LOCK_UN)
        _ = flock(parent, LOCK_UN)
        return (rootURL.appendingPathComponent(id, isDirectory: true), durable)
    }
    func discard() throws {
        if published { return }
        if !removed {
            try validate()
            visited = 0
            try walk(root, removing: true)
            try validate()
            guard unlinkat(parent, name, AT_REMOVEDIR) == 0 else { throw Failure.io }
            removed = true
        }
        guard fsync(parent) == 0 else { throw Failure.io }
        if let journal, let reference = journalReference { try journal.retire(reference) }
        published = true // no longer owns a live name
        _ = flock(parent, LOCK_UN)
    }

    private func walk(_ fd: Int32, removing: Bool, depth: Int = 0, prefix: String = "") throws {
        guard depth <= 34 else { throw Failure.io }
        if removing { guard fchmod(fd, 0o700) == 0 else { throw Failure.io } }
        let copy = fcntl(fd, F_DUPFD_CLOEXEC, 3)
        guard copy >= 0 else { throw Failure.io }
        guard let stream = fdopendir(copy) else { Darwin.close(copy); throw Failure.io }
        defer { closedir(stream) }
        // A dup shares the directory offset; rewind for subsequent freeze,
        // failed-publish cleanup and retry walks on this still-owned descriptor.
        rewinddir(stream)
        while true {
            errno = 0
            guard let entry = readdir(stream) else { guard errno == 0 else { throw Failure.io }; break }
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) }
            }
            if name == "." || name == ".." { continue }
            visited += 1
            guard visited <= 8192 else { throw Failure.io }
            var value = stat()
            guard fstatat(fd, name, &value, AT_SYMLINK_NOFOLLOW) == 0 else { throw Failure.io }
            let path = prefix.isEmpty ? name : prefix + "/" + name
            if value.st_mode & S_IFMT == S_IFDIR {
                if !removing { guard directories.contains(path) else { throw Failure.changed } }
                let child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard child >= 0 else { throw Failure.io }
                do { try walk(child, removing: removing, depth: depth + 1, prefix: path) } catch { Darwin.close(child); throw error }
                Darwin.close(child)
                if removing && unlinkat(fd, name, AT_REMOVEDIR) != 0 { throw Failure.io }
            } else if removing {
                guard unlinkat(fd, name, 0) == 0 else { throw Failure.io }
            } else {
                guard value.st_mode & S_IFMT == S_IFREG, value.st_mode & 0o222 == 0 else { throw Failure.io }
                guard files[path] == FileStamp(value) else { throw Failure.changed }
                verifiedFiles += 1
            }
        }
        if !removing { guard fchmod(fd, 0o500) == 0, fsync(fd) == 0 else { throw Failure.io } }
    }
}

// SSH configuration persistence. The editor owns sparse edits; this store owns
// exact snapshots, mandatory backups and one same-directory atomic commit.
import Darwin
import Foundation

public enum ConfigStoreError: Error, CustomStringConvertible {
    case conflict(String)
    case io(String)
    case committed(String)

    public var description: String {
        switch self {
        case .conflict(let detail):
            return "配置文件已被外部修改（\(detail)）——请重新加载后再编辑"
        case .io(let detail):
            return "写入失败：\(detail)"
        case .committed(let detail):
            return "配置已替换，但同步或回读失败：\(detail)；请重新加载核对，不要重复保存"
        }
    }
}

public final class ConfigStore {
    // Injectable system calls keep failure tests on real disposable files.
    struct FileOperations {
        var write: (Int32, UnsafeRawPointer, Int) -> Int = { Darwin.write($0, $1, $2) }
        var sync: (Int32) -> Int32 = { Darwin.fsync($0) }
        var close: (Int32) -> Int32 = { Darwin.close($0) }
        var rename: (Int32, String, String, Bool) -> Int32 = { directory, source, target, replace in
            if replace { return Darwin.renameat(directory, source, directory, target) }
            return Darwin.renameatx_np(directory, source, directory, target, UInt32(RENAME_EXCL))
        }
    }

    private struct Identity: Equatable {
        let device: dev_t
        let inode: ino_t
        let mode: mode_t
        let owner: uid_t
        let group: gid_t
        let size: off_t
        let modifiedSeconds: Int
        let modifiedNanoseconds: Int
        let changedSeconds: Int
        let changedNanoseconds: Int

        init(_ info: stat) {
            device = info.st_dev; inode = info.st_ino; mode = info.st_mode
            owner = info.st_uid; group = info.st_gid; size = info.st_size
            modifiedSeconds = info.st_mtimespec.tv_sec
            modifiedNanoseconds = info.st_mtimespec.tv_nsec
            changedSeconds = info.st_ctimespec.tv_sec
            changedNanoseconds = info.st_ctimespec.tv_nsec
        }
    }

    private enum Snapshot: Equatable {
        case missing
        case file(Data, Identity)
    }

    private let path: String
    private let backupName: String
    private let operations: FileOperations
    private var loaded: Result<Snapshot, Error>

    public convenience init(path: String) {
        self.init(path: path, operations: FileOperations())
    }

    init(path: String, operations: FileOperations, backupName: String? = nil) {
        self.path = path
        self.backupName = backupName ?? ((URL(fileURLWithPath: path).lastPathComponent as NSString).deletingPathExtension + ".bak")
        self.operations = operations
        // Preserve read errors: an unreadable/nonregular file is not missing.
        loaded = Result { try Self.snapshot(directory: AT_FDCWD, name: path) }
    }

    private static func ioError() -> Error {
        POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }

    private static func snapshot(directory: Int32, name: String) throws -> Snapshot {
        let fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
        if fd < 0, errno == ENOENT { return .missing }
        guard fd >= 0 else { throw ioError() }
        defer { Darwin.close(fd) }
        var before = stat()
        guard fstat(fd, &before) == 0 else { throw ioError() }
        guard before.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG) else {
            throw ConfigStoreError.io("配置必须是非链接的常规文件")
        }
        var bytes = Data(), buffer = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if count < 0, errno == EINTR { continue }
            guard count >= 0 else { throw ioError() }
            if count == 0 { break }
            bytes.append(contentsOf: buffer.prefix(count))
        }
        var after = stat(), current = stat()
        guard fstat(fd, &after) == 0,
              fstatat(directory, name, &current, AT_SYMLINK_NOFOLLOW) == 0 else { throw ioError() }
        guard Identity(before) == Identity(after), Identity(after) == Identity(current),
              after.st_size == bytes.count else {
            throw ConfigStoreError.conflict("读取期间文件发生变化")
        }
        return .file(bytes, Identity(after))
    }

    /// Consumers of other native text configurations share the same captured
    /// identity/bytes transaction; nil means ENOENT, while every read error throws.
    func loadedBytes() throws -> Data? {
        switch try loaded.get() {
        case .missing: return nil
        case .file(let bytes, _): return bytes
        }
    }

    public func isExternallyModified() -> Bool {
        guard let expected = try? loaded.get(),
              let current = try? Self.snapshot(directory: AT_FDCWD, name: path) else { return true }
        return current != expected
    }

    /// Failure before rename preserves the target. A later failure explicitly
    /// reports the committed effect and requires a reload before another save.
    public func save(_ text: String) throws {
        let expected: Snapshot
        do { expected = try loaded.get() }
        catch { throw ConfigStoreError.io("加载快照失败，请重新加载：\(error)") }
        let url = URL(fileURLWithPath: path)
        let parent = url.deletingLastPathComponent().path
        let directory = Darwin.open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard directory >= 0 else { throw ConfigStoreError.io("打开配置目录失败：\(Self.ioError())") }
        defer { Darwin.close(directory) }
        let name = url.lastPathComponent
        // A stable advisory lock serializes our writers. Never unlink it:
        // removing a lock name would let a second writer lock another inode.
        let lockName = ".\(name).lock"
        let lock = openat(directory, lockName, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0o600)
        guard lock >= 0 else { throw ConfigStoreError.io("打开保存协调文件失败：\(Self.ioError())") }
        var locked = false
        defer { if locked { flock(lock, LOCK_UN) }; Darwin.close(lock) }
        var lockIdentity = stat()
        guard fstat(lock, &lockIdentity) == 0,
              lockIdentity.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG),
              lockIdentity.st_uid == geteuid(), lockIdentity.st_nlink == 1,
              lockIdentity.st_mode & 0o077 == 0 else {
            throw ConfigStoreError.io("保存协调文件必须是当前用户独占的私有常规文件")
        }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else {
            throw ConfigStoreError.io("无法取得保存锁，请稍后重试：\(Self.ioError())")
        }
        locked = true
        let bytes = Data(text.utf8)
        var committed = false
        var temporary: [String] = []
        defer { for item in temporary { unlinkat(directory, item, 0) } }

        func verify() throws {
            var held = stat(), current = stat(), currentLock = stat()
            guard fstat(directory, &held) == 0, stat(parent, &current) == 0 else { throw Self.ioError() }
            guard held.st_dev == current.st_dev, held.st_ino == current.st_ino else {
                throw ConfigStoreError.conflict("配置目录已改变")
            }
            guard fstatat(directory, lockName, &currentLock, AT_SYMLINK_NOFOLLOW) == 0,
                  currentLock.st_dev == lockIdentity.st_dev, currentLock.st_ino == lockIdentity.st_ino else {
                throw ConfigStoreError.conflict("保存协调文件已改变")
            }
            guard try Self.snapshot(directory: directory, name: name) == expected else {
                throw ConfigStoreError.conflict("内容或文件身份与加载快照不一致")
            }
        }

        func stage(_ data: Data, identity: Identity?) throws -> String {
            let temporaryName = ".\(name).tmp-\(UUID().uuidString)"
            let fd = openat(directory, temporaryName, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600)
            guard fd >= 0 else { throw Self.ioError() }
            temporary.append(temporaryName)
            var closed = false
            defer { if !closed { Darwin.close(fd) } }
            try data.withUnsafeBytes { raw in
                var offset = 0
                while offset < raw.count {
                    let count = operations.write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                    if count < 0, errno == EINTR { continue }
                    guard count > 0 else { throw Self.ioError() }
                    offset += count
                }
            }
            if let identity {
                guard fchown(fd, identity.owner, identity.group) == 0,
                      fchmod(fd, identity.mode & 0o777) == 0 else { throw Self.ioError() }
            }
            guard operations.sync(fd) == 0 else { throw Self.ioError() }
            closed = true
            guard operations.close(fd) == 0 else { throw Self.ioError() }
            return temporaryName
        }

        do {
            try verify()
            var identity: Identity?
            if case .file(let original, let info) = expected {
                identity = info
                let backup = backupName
                let stagedBackup = try stage(original, identity: nil)
                guard operations.rename(directory, stagedBackup, backup, true) == 0,
                      operations.sync(directory) == 0 else { throw Self.ioError() }
            }
            let staged = try stage(bytes, identity: identity)
            try verify()
            guard operations.rename(directory, staged, name, identity != nil) == 0 else {
                if errno == EEXIST { throw ConfigStoreError.conflict("保存期间目标被创建") }
                throw Self.ioError()
            }
            committed = true
            // Invalidate before fallible post-commit work; retry cannot assume
            // the old file remains, even when a subsequent sync/read fails.
            loaded = .failure(ConfigStoreError.committed("需要重新加载"))
            guard operations.sync(directory) == 0 else { throw Self.ioError() }
            let refreshed = try Self.snapshot(directory: directory, name: name)
            guard case .file(let actual, _) = refreshed, actual == bytes else {
                throw ConfigStoreError.conflict("替换后内容再次变化")
            }
            loaded = .success(refreshed)
        } catch {
            if committed { throw ConfigStoreError.committed("\(error)") }
            if case ConfigStoreError.conflict = error { throw error }
            throw ConfigStoreError.io("\(error)")
        }
    }
}

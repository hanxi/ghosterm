import CryptoKit
import Darwin
import Foundation

public struct ExtensionLocalJournalEntry: Sendable, Identifiable {
    public enum Location: String, Sendable { case matching, missing, changed, unbound, unavailable }
    public let id, snapshot, directory: String
    public let reservedBytes: UInt64
    public let held: Bool
    public let location: Location
}

/// Durable admission reservations, not a filesystem quota or process recovery.
/// Only Native-selected storage is accepted; no prefix discovery or directory
/// deletion is performed here. All calls run on the controller's IO worker.
final class ExtensionLocalJournal {
    typealias Failure = ExtensionLocalTemporaryDirectory.Failure
    static let reservation: UInt64 = 256 * 1024 * 1024
    static let budget: UInt64 = 1024 * 1024 * 1024
    private static let marker = Data("Ghosterm Native local reservations 1\n".utf8)
    fileprivate struct Identity: Codable, Equatable {
        let device: Int32, inode: UInt64, uid: UInt32, birthSeconds: Int64, birthNanoseconds: Int64
        init(_ value: stat) {
            device = value.st_dev; inode = value.st_ino; uid = value.st_uid
            birthSeconds = Int64(value.st_birthtimespec.tv_sec); birthNanoseconds = Int64(value.st_birthtimespec.tv_nsec)
        }
    }
    fileprivate struct Record: Codable {
        let version: Int, id, base, name: String
        let parent: Identity
        var namespace: Identity?
        let reservedBytes: UInt64
    }
    final class Lease {
        fileprivate let journal: ExtensionLocalJournal
        fileprivate var record: Record
        fileprivate var fd: Int32
        fileprivate var snapshot: String
        fileprivate var retired = false
        fileprivate init(_ journal: ExtensionLocalJournal, _ record: Record, _ fd: Int32, _ snapshot: String) {
            self.journal = journal; self.record = record; self.fd = fd; self.snapshot = snapshot
        }
        deinit { if fd >= 0 { Darwin.close(fd) } }
        func bind(_ value: stat) throws { try journal.bind(self, value) }
        func finish() throws { try journal.finish(self) }
    }
    private let path: String
    private var root: Int32 = -1
    private var identity: Identity?

    init(directory: URL) throws {
        guard directory.isFileURL, directory.path.hasPrefix("/"), directory.path.utf8.count <= 4096,
              !directory.path.utf8.contains(0), directory.lastPathComponent != ".", directory.lastPathComponent != "/" else {
            throw Failure(EINVAL, "本机运行记录目录无效")
        }
        path = directory.path
        let parent = open(directory.deletingLastPathComponent().path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard parent >= 0 else { throw Failure(errno, "无法打开本机运行记录的父目录") }
        defer { Darwin.close(parent) }
        let created = mkdirat(parent, directory.lastPathComponent, 0o700) == 0
        guard created || errno == EEXIST else { throw Failure(errno, "无法创建本机运行记录目录") }
        root = openat(parent, directory.lastPathComponent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        do {
            var value = stat()
            guard root >= 0, fstat(root, &value) == 0, value.st_uid == geteuid(), value.st_mode & 0o077 == 0 else {
                throw Failure(EPERM, "本机运行记录目录的类型或权限不符")
            }
            identity = Identity(value)
            if created {
                let marker = try create("format", data: Self.marker)
                Darwin.close(marker)
                let lock = try create("lock", data: Data()); Darwin.close(lock)
                guard fsync(root) == 0, fsync(parent) == 0 else { throw Failure(errno, "本机运行记录目录未能持久化") }
            } else {
                let marker = try read("format"); defer { Darwin.close(marker.fd) }
                guard marker.data == Self.marker else { throw Failure(EINVAL, "本机运行记录格式未知，拒绝接管") }
            }
            guard fsync(root) == 0, fsync(parent) == 0 else { throw Failure(errno, "本机运行记录目录未能持久化") }
        } catch { if root >= 0 { Darwin.close(root); root = -1 }; throw error }
    }
    deinit { if root >= 0 { Darwin.close(root) } }
    private func validate() throws {
        var value = stat()
        let fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw Failure(ESTALE, "本机运行记录目录位置已变化") }
        defer { Darwin.close(fd) }
        guard fstat(fd, &value) == 0, Identity(value) == identity, value.st_mode & 0o077 == 0,
              fstat(root, &value) == 0, Identity(value) == identity else { throw Failure(ESTALE, "本机运行记录目录已被替换") }
        let marker = try read("format"); defer { Darwin.close(marker.fd) }
        guard marker.data == Self.marker else { throw Failure(EINVAL, "本机运行记录格式已变化") }
    }
    private func checked(_ fd: Int32) throws -> stat {
        var value = stat()
        guard fd >= 0, fstat(fd, &value) == 0, value.st_mode & S_IFMT == S_IFREG,
              value.st_uid == geteuid(), value.st_mode & 0o077 == 0, value.st_nlink == 1,
              value.st_size >= 0, value.st_size <= 16384 else { throw Failure(EPERM, "本机运行记录的类型、权限或大小不符") }
        return value
    }
    private func read(_ name: String) throws -> (fd: Int32, data: Data) {
        let fd = openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
        do {
            let before = try checked(fd)
            var data = Data(), bytes = [UInt8](repeating: 0, count: 4096)
            while true {
                let n = Darwin.read(fd, &bytes, bytes.count)
                if n < 0 && errno == EINTR { continue }
                guard n >= 0, data.count <= 16384 - n else { throw Failure(EIO, "读取本机运行记录失败或超限") }
                if n == 0 { break }; data.append(contentsOf: bytes.prefix(n))
            }
            let after = try checked(fd)
            guard Identity(before) == Identity(after), before.st_size == after.st_size,
                  before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec,
                  before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec, data.count == before.st_size else {
                throw Failure(ESTALE, "读取期间本机运行记录发生变化")
            }
            return (fd, data)
        } catch { if fd >= 0 { Darwin.close(fd) }; throw error }
    }
    private func create(_ name: String, data: Data) throws -> Int32 {
        guard data.count <= 16384 else { throw Failure(EFBIG, "本机运行记录超限") }
        let fd = openat(root, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw Failure(errno, "无法独占创建本机运行记录") }
        do {
            try data.withUnsafeBytes { bytes in
                var offset = 0
                while offset < bytes.count {
                    let n = Darwin.write(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                    if n < 0 && errno == EINTR { continue }
                    guard n > 0 else { throw Failure(EIO, "无法完整写入本机运行记录") }; offset += n
                }
            }
            guard fsync(fd) == 0 else { throw Failure(errno, "本机运行记录未能持久化") }
            return fd
        } catch { Darwin.close(fd); throw error }
    }
    private func locked<T>(_ body: () throws -> T) throws -> T {
        try validate()
        let lock = openat(root, "lock", O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
        defer { if lock >= 0 { Darwin.close(lock) } }
        _ = try checked(lock)
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else { throw Failure(errno, "另一个本机运行记录操作尚未结束") }
        try validate(); return try body()
    }
    private func records() throws -> [(Record, String)] {
        let copy = openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard copy >= 0 else { throw Failure(errno, "无法列举本机运行记录") }
        guard let stream = fdopendir(copy) else { Darwin.close(copy); throw Failure(errno, "无法读取本机运行记录目录") }
        defer { closedir(stream) }
        var result: [(Record, String)] = [], count = 0
        while true {
            errno = 0
            guard let entry = readdir(stream) else {
                guard errno == 0 else { throw Failure(errno, "列举本机运行记录失败") }; break
            }
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) { String(validatingUTF8: $0) }
            }
            guard let name else { throw Failure(EINVAL, "本机运行记录名称无效") }
            if name == "." || name == ".." { continue }
            count += 1
            guard count <= 6 else { throw Failure(EFBIG, "本机运行记录数量超限，保留供检查") }
            if name == "format" || name == "lock" { continue }
            guard name.hasSuffix(".json"), validID(String(name.dropLast(5))) else {
                throw Failure(EINVAL, "存在不完整或未知本机运行记录，保留供检查")
            }
            let file = try read(name); defer { Darwin.close(file.fd) }
            let record = try JSONDecoder().decode(Record.self, from: file.data)
            guard record.version == 1, validID(record.id), name == record.id + ".json",
                  record.name == "ghosterm-local-" + record.id, record.reservedBytes == Self.reservation,
                  record.base.hasPrefix("/"), record.base.utf8.count <= 4096, !record.base.utf8.contains(0) else {
                throw Failure(EINVAL, "本机运行记录内容不符，拒绝接管")
            }
            result.append((record, SHA256.hash(data: file.data).map { String(format: "%02x", $0) }.joined()))
        }
        return result.sorted { $0.0.id < $1.0.id }
    }
    private func validID(_ value: String) -> Bool {
        value.utf8.count == 32 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    private func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    func reserve(parent: Int32, base: String, id: String) throws -> Lease {
        try locked {
            let current = try records()
            guard UInt64(current.count) < Self.budget / Self.reservation else { throw Failure(ENOSPC, "本机运行暂存预约已满；保留记录仍占用额度") }
            var value = stat()
            guard validID(id), fstat(parent, &value) == 0, value.st_mode & S_IFMT == S_IFDIR,
                  base.hasPrefix("/"), base.utf8.count <= 4096, !base.utf8.contains(0) else { throw Failure(EINVAL, "临时根目录身份无效") }
            let record = Record(version: 1, id: id, base: base, name: "ghosterm-local-" + id,
                parent: Identity(value), namespace: nil, reservedBytes: Self.reservation)
            let data = try JSONEncoder().encode(record), fd = try create(id + ".json", data: data)
            guard flock(fd, LOCK_EX | LOCK_NB) == 0, fsync(root) == 0 else {
                let code = errno; Darwin.close(fd); throw Failure(code, "本机运行预约未能持久化，拒绝创建临时目录")
            }
            return Lease(self, record, fd, digest(data))
        }
    }
    private func exact(_ lease: Lease) throws {
        let held = try checked(lease.fd)
        var value = stat()
        guard fstatat(root, lease.record.id + ".json", &value, AT_SYMLINK_NOFOLLOW) == 0,
              Identity(value) == Identity(held), value.st_size == held.st_size else {
            throw Failure(ESTALE, "本机运行预约记录已变化")
        }
        let file = try read(lease.record.id + ".json"); defer { Darwin.close(file.fd) }
        let observed = try checked(file.fd)
        guard Identity(observed) == Identity(held), digest(file.data) == lease.snapshot else {
            throw Failure(ESTALE, "本机运行预约记录内容已变化")
        }
    }
    private func bind(_ lease: Lease, _ value: stat) throws {
        try locked {
            try exact(lease)
            guard lease.record.namespace == nil, value.st_mode & S_IFMT == S_IFDIR, value.st_uid == geteuid() else { throw Failure(ESTALE, "本机临时目录不能绑定到预约") }
            var record = lease.record; record.namespace = Identity(value)
            let data = try JSONEncoder().encode(record), fd = try create("pending", data: data)
            do {
                guard flock(fd, LOCK_EX | LOCK_NB) == 0, renameat(root, "pending", root, record.id + ".json") == 0 else { throw Failure(errno, "本机临时目录归属记录未能提交") }
                Darwin.close(lease.fd); lease.fd = fd; lease.record = record; lease.snapshot = digest(data)
                guard fsync(root) == 0 else { throw Failure(errno, "本机临时目录归属记录未能持久化") }
            } catch { if lease.fd != fd { Darwin.close(fd) }; throw error }
        }
    }
    private func finish(_ lease: Lease) throws {
        try locked {
            if !lease.retired {
                try exact(lease)
                guard unlinkat(root, lease.record.id + ".json", 0) == 0 else { throw Failure(errno, "无法释放本机运行预约") }
                lease.retired = true
            }
            guard fsync(root) == 0 else { throw Failure(errno, "本机运行预约释放未能持久化") }
            if lease.fd >= 0 { Darwin.close(lease.fd); lease.fd = -1 }
        }
    }
    private func location(_ record: Record) -> ExtensionLocalJournalEntry.Location {
        let parent = open(record.base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard parent >= 0 else { return .unavailable }; defer { Darwin.close(parent) }
        var value = stat()
        guard fstat(parent, &value) == 0, Identity(value) == record.parent else { return .changed }
        guard fstatat(parent, record.name, &value, AT_SYMLINK_NOFOLLOW) == 0 else { return errno == ENOENT ? .missing : .unavailable }
        guard let identity = record.namespace else { return .unbound }
        return value.st_mode & S_IFMT == S_IFDIR && Identity(value) == identity ? .matching : .changed
    }
    func inspect() throws -> [ExtensionLocalJournalEntry] {
        try locked {
            try records().map { record, snapshot in
                let file = try read(record.id + ".json"); defer { Darwin.close(file.fd) }
                guard digest(file.data) == snapshot else { throw Failure(ESTALE, "检查期间预约记录已变化") }
                let held = flock(file.fd, LOCK_EX | LOCK_NB) != 0
                guard !held || errno == EWOULDBLOCK else { throw Failure(errno, "无法核对本机运行预约占用") }
                return .init(id: record.id, snapshot: snapshot, directory: record.base + "/" + record.name,
                    reservedBytes: record.reservedBytes, held: held, location: location(record))
            }
        }
    }
    func releaseMissing(id: String, snapshot: String, confirmed: Bool) throws {
        try locked {
            guard confirmed, let (record, current) = try records().first(where: { $0.0.id == id }), current == snapshot else { throw Failure(ESTALE, "需要重新检查并确认该预约记录") }
            let file = try read(id + ".json"); defer { Darwin.close(file.fd) }
            guard flock(file.fd, LOCK_EX | LOCK_NB) == 0 else { throw Failure(EBUSY, "本机运行仍占有该预约") }
            guard location(record) == .missing else { throw Failure(ESTALE, "目录尚未确认消失，保留预约") }
            let lease = Lease(self, record, file.fd, current)
            defer { lease.fd = -1 }
            try exact(lease)
            guard unlinkat(root, id + ".json", 0) == 0, fsync(root) == 0 else { throw Failure(errno, "预约释放未能持久化，请重新检查") }
        }
    }
}

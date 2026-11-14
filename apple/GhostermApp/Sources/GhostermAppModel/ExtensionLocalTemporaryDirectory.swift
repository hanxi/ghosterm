import Darwin
import Foundation

public struct ExtensionLocalTemporaryState: Sendable {
    public enum Phase: String, Sendable { case active, removing, retained, removed }
    public let directory: String
    public let phase: Phase
    public let error: Int32
    public let detail: String
}

/// One newly created Native-owned namespace, never an adopted user directory.
/// The execution decision includes removal of all temporary names under it.
/// Called only on the IO worker, after the transport owner has been released.
/// No recovery by prefix, process discovery, or same-UID sandbox is implied.
final class ExtensionLocalTemporaryDirectory {
    private final class Frame {
        var stream: UnsafeMutablePointer<DIR>?
        let identity: stat
        let name: String
        init(_ stream: UnsafeMutablePointer<DIR>, _ identity: stat, _ name: String) {
            self.stream = stream; self.identity = identity; self.name = name
        }
        var fd: Int32 { stream.map { dirfd($0) } ?? -1 }
        func close() throws {
            guard let stream else { return }; self.stream = nil
            guard closedir(stream) == 0 else { throw Failure(errno, "关闭临时目录读取句柄失败") }
        }
        deinit { if let stream { closedir(stream) } }
    }
    struct Failure: Error { let code: Int32; let detail: String
        init(_ code: Int32, _ detail: String) { self.code = code; self.detail = detail }
    }
    let directory: String
    private let base, name: String
    private var parent: Int32 = -1, root: Int32 = -1
    private var identity: stat?
    private var reservation: ExtensionLocalJournal.Lease?
    private var namespaceRemoved = false
    private var frames: [Frame] = []
    private var entries = 0
    private var phase: ExtensionLocalTemporaryState.Phase = .active
    private var error: Int32 = 0
    private var detail = "本次运行的私有临时目录，结束后清理"
    var observation: ExtensionLocalTemporaryState {
        .init(directory: directory, phase: phase, error: error, detail: detail)
    }

    init(parentFD: Int32, base: String, journal: ExtensionLocalJournal) throws {
        self.base = base
        let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        name = "ghosterm-local-" + id
        directory = base + (base.hasSuffix("/") ? "" : "/") + name
        guard directory.utf8.count <= 4096 else { throw Failure(ENAMETOOLONG, "私有临时目录路径过长") }
        parent = fcntl(parentFD, F_DUPFD_CLOEXEC, 3)
        guard parent >= 0 else { throw Failure(errno, "无法保留临时根目录") }
        do { reservation = try journal.reserve(parent: parent, base: base, id: id) }
        catch { Darwin.close(parent); parent = -1; throw error }
        guard name.withCString({ mkdirat(parent, $0, 0o700) }) == 0 else {
            let code = errno; Darwin.close(parent); parent = -1
            throw Failure(code, "无法创建私有临时目录")
        }
        // Once mkdir succeeds, return an owner even if later opening fails.
        // The caller must retain it and expose failure, never lose the name.
        do {
            var value = stat()
            guard name.withCString({ fstatat(parent, $0, &value, AT_SYMLINK_NOFOLLOW) }) == 0,
                  value.st_mode & S_IFMT == S_IFDIR, value.st_uid == geteuid(), value.st_mode & 0o077 == 0 else {
                throw Failure(ESTALE, "新建私有临时目录的归属无法确认，保留供检查")
            }
            identity = value
            try reservation?.bind(value)
            root = name.withCString { openat(parent, $0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
            guard root >= 0 else { throw Failure(errno, "无法打开新建私有临时目录") }
            try validate()
        } catch { retain(error) }
    }
    deinit {
        frames.removeAll()
        if root >= 0 { Darwin.close(root) }
        if parent >= 0 { Darwin.close(parent) }
    }
    func executionPath() throws -> String {
        guard phase == .active else { throw Failure(error, detail) }
        try validate(); return directory
    }
    private func same(_ value: stat, _ expected: stat) -> Bool {
        value.st_dev == expected.st_dev && value.st_ino == expected.st_ino &&
            value.st_mode & S_IFMT == expected.st_mode & S_IFMT && value.st_uid == geteuid()
    }
    private func validate() throws {
        guard let identity else { throw Failure(ESTALE, "私有临时目录归属未知，拒绝清理") }
        var value = stat()
        guard parent >= 0, name.withCString({ fstatat(parent, $0, &value, AT_SYMLINK_NOFOLLOW) }) == 0,
              same(value, identity) else { throw Failure(ESTALE, "私有临时目录位置已变化，保留供检查") }
        if root < 0 {
            root = name.withCString { openat(parent, $0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
        }
        guard root >= 0, fstat(root, &value) == 0, same(value, identity) else {
            throw Failure(ESTALE, "私有临时目录句柄不再匹配")
        }
        let pathFD = directory.withCString { open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
        guard pathFD >= 0 else { throw Failure(ESTALE, "私有临时目录路径不再匹配") }
        defer { Darwin.close(pathFD) }
        guard fstat(pathFD, &value) == 0, same(value, identity) else {
            throw Failure(ESTALE, "私有临时目录路径已被替换")
        }
        for i in frames.indices.dropFirst() {
            let frame = frames[i]
            guard frame.name.withCString({ fstatat(frames[i - 1].fd, $0, &value, AT_SYMLINK_NOFOLLOW) }) == 0,
                  same(value, frame.identity) else { throw Failure(ESTALE, "临时子目录位置已变化") }
        }
    }
    private func openFrame(_ parent: Int32, name: String, expected: stat) throws -> Frame {
        let fd = name.withCString { openat(parent, $0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
        guard fd >= 0 else { throw Failure(errno, "无法打开临时子目录") }
        var value = stat()
        guard fstat(fd, &value) == 0, same(value, expected), value.st_mode & S_IFMT == S_IFDIR else {
            Darwin.close(fd); throw Failure(ESTALE, "临时子目录身份已变化")
        }
        guard let identity, value.st_dev == identity.st_dev else {
            Darwin.close(fd); throw Failure(EXDEV, "临时目录包含其它文件系统，保留供检查")
        }
        // Staged package directories are sealed 0500; only our namespace may
        // be made writable. Never chmod files (which could be hard links).
        guard fchmod(fd, 0o700) == 0 else {
            let code = errno; Darwin.close(fd); throw Failure(code, "无法准备私有临时目录清理")
        }
        guard let stream = fdopendir(fd) else {
            let code = errno; Darwin.close(fd); throw Failure(code, "无法读取私有临时目录")
        }
        return Frame(stream, value, name)
    }
    private func retain(_ value: Error) {
        let failure = value as? Failure
        phase = .retained; error = failure?.code ?? EIO; detail = failure?.detail ?? "临时目录清理失败，保留供重试"
        frames.removeAll()
    }
    /// At most 64 entries per step, 64 directory levels and 16,384 entries per
    /// attempt. A bound/replacement/IO error retains ownership; no prefix scan
    /// or fallback to recursive Foundation deletion. Kernel FS latency is not bounded.
    func closeStep() throws -> Bool {
        if phase == .removed { return true }
        do {
            if namespaceRemoved { return try finishRemoval() }
            try validate()
            if frames.isEmpty {
                guard let identity else { throw Failure(ESTALE, "临时目录归属未知") }
                frames.append(try openFrame(root, name: ".", expected: identity))
                phase = .removing; error = 0; detail = "正在清理本次私有临时目录"; entries = 0
            }
            for _ in 0..<64 {
                try validate()
                guard let frame = frames.last, let stream = frame.stream else { throw Failure(EIO, "临时目录读取状态丢失") }
                errno = 0
                guard let entry = readdir(stream) else {
                    guard errno == 0 else { throw Failure(errno, "读取私有临时目录失败") }
                    let target = frames.count == 1 ? parent : frames[frames.count - 2].fd
                    let leaf = frames.count == 1 ? name : frame.name
                    var value = stat()
                    guard leaf.withCString({ fstatat(target, $0, &value, AT_SYMLINK_NOFOLLOW) }) == 0,
                          same(value, frame.identity) else { throw Failure(ESTALE, "清理前临时目录已被替换") }
                    guard leaf.withCString({ unlinkat(target, $0, AT_REMOVEDIR) }) == 0 else {
                        throw Failure(errno, "私有临时目录仍有内容或无法移除")
                    }
                    if frames.count == 1 { namespaceRemoved = true }
                    try frame.close(); frames.removeLast()
                    if frames.isEmpty {
                        return try finishRemoval()
                    }
                    continue
                }
                let leaf = withUnsafePointer(to: &entry.pointee.d_name) {
                    $0.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) { String(validatingUTF8: $0) }
                }
                guard let leaf else { throw Failure(EILSEQ, "临时文件名不是受支持的 UTF-8，保留供检查") }
                if leaf == "." || leaf == ".." { continue }
                entries += 1
                guard entries <= 16384 else { throw Failure(EFBIG, "临时文件超过单次清理限额，保留供检查") }
                var value = stat()
                guard leaf.withCString({ fstatat(frame.fd, $0, &value, AT_SYMLINK_NOFOLLOW) }) == 0,
                      value.st_uid == geteuid() else { throw Failure(ESTALE, "临时文件归属或位置已变化") }
                if value.st_mode & S_IFMT == S_IFDIR {
                    guard frames.count < 64 else { throw Failure(ELOOP, "临时目录超过清理深度限额") }
                    frames.append(try openFrame(frame.fd, name: leaf, expected: value))
                } else {
                    guard [S_IFREG, S_IFLNK, S_IFIFO, S_IFSOCK].contains(value.st_mode & S_IFMT) else {
                        throw Failure(ENOTSUP, "临时目录包含不支持清理的文件类型")
                    }
                    try validate()
                    var current = stat()
                    guard leaf.withCString({ fstatat(frame.fd, $0, &current, AT_SYMLINK_NOFOLLOW) }) == 0,
                          same(current, value) else { throw Failure(ESTALE, "清理前临时文件已被替换") }
                    guard leaf.withCString({ unlinkat(frame.fd, $0, 0) }) == 0 else {
                        throw Failure(errno, "无法移除本次私有临时文件")
                    }
                }
            }
            return false
        } catch { retain(error); throw error }
    }
    private func finishRemoval() throws -> Bool {
        try reservation?.finish(); reservation = nil
        phase = .removed; error = 0; detail = "本次私有临时目录与预约已移除"
        let owned = [root, parent]; root = -1; parent = -1
        for fd in owned where fd >= 0 {
            if Darwin.close(fd) != 0, error == 0 {
                error = errno; detail = "私有临时目录已移除，但句柄释放结果不明"
            }
        }
        return true
    }
}

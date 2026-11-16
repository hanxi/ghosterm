import CryptoKit
import Darwin
import Foundation

/// Explicit, fd-anchored maintenance of one confirmed tree. No prefix adoption,
/// startup replay, recursive FileManager deletion, or permission restoration.
final class PluginReleaseRemoval {
    enum Failure: String, Error, LocalizedError {
        case enabled, occupied, pending, started, changed, invalid, io
        var errorDescription: String? {
            switch self {
            case .enabled: return "请先禁用此版本的新建，再预览回收。"
            case .occupied: return "此版本仍有运行引用或另一项回收预览，请先停止或取消后重试。"
            case .pending: return "已有确认过的删除记录，请先显式继续处理。"
            case .started: return "此删除已进入移除阶段，不能撤销；请核对记录后继续处理。"
            case .changed: return "目录或清单内容已变化，已保留文件；请重新核对。"
            case .invalid: return "回收记录或目录类型无效，不能据名称删除。"
            case .io: return "回收未完成；请刷新并核对保留的删除记录。"
            }
        }
    }
    struct Stamp: Codable, Equatable, Sendable {
        let device, inode, uid, mode, links, size, modified, changed, born: String
        init(_ s: stat) {
            device = String(s.st_dev); inode = String(s.st_ino); uid = String(s.st_uid)
            mode = String(s.st_mode); links = String(s.st_nlink); size = String(s.st_size)
            modified = "\(s.st_mtimespec.tv_sec):\(s.st_mtimespec.tv_nsec)"
            changed = "\(s.st_ctimespec.tv_sec):\(s.st_ctimespec.tv_nsec)"
            born = "\(s.st_birthtimespec.tv_sec):\(s.st_birthtimespec.tv_nsec)"
        }
        func sameObject(_ other: Stamp) -> Bool {
            device == other.device && inode == other.inode && uid == other.uid && born == other.born &&
                UInt16(mode).map { $0 & S_IFMT } == UInt16(other.mode).map { $0 & S_IFMT }
        }
    }
    struct Member: Codable, Equatable, Sendable {
        let path: String
        let directory: Bool
        fileprivate let stamp: Stamp
        let sha256: String?
        var bytes: Int64 { directory ? 0 : (Int64(stamp.size) ?? 0) }
    }
    struct Header: Codable, Equatable {
        let schema: Int
        let operation, releaseID, tombstone: String
        let revision: String?
        let root, version: Stamp
    }
    struct Result: Sendable, Equatable {
        let releaseID: String
        let removed: Int
        let observedAbsent: Int
        let complete: Bool
        let durabilityConfirmed: Bool
    }
    let id = UUID().uuidString
    let directory: URL
    let members: [Member]
    private let root: Int32
    private let version: PluginReleaseCatalog.VersionHandle
    private let header: Header
    private var confirmed = false
    private let synchronize: (Int32) -> Int32
    private let checkpoint: (String) throws -> Void
    var releaseID: String { header.releaseID }
    var bytes: Int64 { members.reduce(0) { $0 + $1.bytes } }
    deinit { Darwin.close(root) }
    static func pendingPreview(owner: PluginReleaseCatalog) throws -> (String, String, URL, [Member])? {
        guard try owner.hasPendingRemoval() else { return nil }
        let root = try owner.removalRoot(); defer { Darwin.close(root) }
        let (header, members, _) = try readIntent(root)
        guard header.root.sameObject(try statFD(root)) else { throw Failure.changed }
        let name = try namedStamp(root, header.tombstone) == nil ? header.releaseID : header.tombstone
        return (header.operation, header.releaseID, owner.url.appendingPathComponent(name), members)
    }
    static func cancelPending(owner: PluginReleaseCatalog, id: String) throws -> Bool {
        let root = try owner.removalRoot(); defer { Darwin.close(root) }
        let (header, members, _) = try readIntent(root)
        guard header.operation == id, header.root.sameObject(try statFD(root)) else { throw Failure.changed }
        if let detached = try namedStamp(root, header.tombstone) {
            guard detached.sameObject(header.version) else { throw Failure.changed }
            throw Failure.started
        }
        let handle = try owner.openVersion(header.releaseID)
        guard try statFD(handle.fd) == header.version,
              try scan(handle.fd, device: header.root.device) == members else { throw Failure.changed }
        try clearIntent(root, expected: header, members: members)
        return fsync(root) == 0
    }

    init(owner: PluginReleaseCatalog, releaseID: String, expectedRevision: String?,
         synchronize: @escaping (Int32) -> Int32 = { fsync($0) }, checkpoint: @escaping (String) throws -> Void = { _ in }) throws {
        guard try !owner.hasPendingRemoval() else { throw Failure.pending }
        let selection = try owner.selection()
        guard selection.revision == expectedRevision else { throw PluginReleaseCatalog.Failure.conflict }
        guard !selection.choices.values.contains(where: { $0.releaseID == releaseID && $0.enabled }) else { throw Failure.enabled }
        let handle = try owner.openVersion(releaseID)
        guard try handle.exclusiveUse() else { throw Failure.occupied }
        let root = try owner.removalRoot()
        do {
            let rootStamp = try Self.statFD(root), versionStamp = try Self.statFD(handle.fd)
            guard rootStamp.device == versionStamp.device else { throw Failure.invalid }
            members = try Self.scan(handle.fd, device: rootStamp.device)
            let operation = UUID().uuidString.lowercased()
            header = Header(schema: 1, operation: operation, releaseID: releaseID,
                tombstone: ".removing-" + operation, revision: selection.revision, root: rootStamp, version: versionStamp)
            self.root = root; version = handle; directory = owner.url.appendingPathComponent(releaseID)
            self.synchronize = synchronize; self.checkpoint = checkpoint
        } catch { Darwin.close(root); throw error }
    }
    func confirm(owner: PluginReleaseCatalog) throws -> Result {
        guard !confirmed else { throw Failure.pending }
        let currentRoot = try owner.removalRoot(); defer { Darwin.close(currentRoot) }
        guard header.root.sameObject(try Self.statFD(currentRoot)), try !owner.hasPendingRemoval() else { throw Failure.changed }
        try Self.requireDisabled(owner, header: header, checkRevision: true)
        guard try Self.namedStamp(root, header.releaseID) == header.version,
              try Self.scan(version.fd, device: header.root.device) == members else { throw Failure.changed }
        try checkpoint("before-intent")
        try Self.writeIntent(root: root, header: header, members: members)
        confirmed = true
        // The name and its contents must survive before the version can move.
        guard synchronize(root) == 0 else { return result(0, 0, false, false) }
        try checkpoint("after-intent")
        return try finish(owner: owner, alreadyDetached: false)
    }
    private init(root: Int32, version: PluginReleaseCatalog.VersionHandle, directory: URL,
                 header: Header, members: [Member], synchronize: @escaping (Int32) -> Int32,
                 checkpoint: @escaping (String) throws -> Void) {
        self.root = root; self.version = version; self.directory = directory; self.header = header
        self.members = members; confirmed = true; self.synchronize = synchronize; self.checkpoint = checkpoint
    }
    /// This entry point is only called by an explicit user maintenance action.
    static func resume(owner: PluginReleaseCatalog, synchronize: @escaping (Int32) -> Int32 = { fsync($0) },
                       checkpoint: @escaping (String) throws -> Void = { _ in }) throws -> Result {
        let root = try owner.removalRoot()
        var transferred = false
        defer { if !transferred { Darwin.close(root) } }
        let (header, members, _) = try readIntent(root)
        guard header.root.sameObject(try statFD(root)) else { throw Failure.changed }
        try requireDisabled(owner, header: header, checkRevision: false)
        let detached = try namedStamp(root, header.tombstone)
        let original = try namedStamp(root, header.releaseID)
        let name: String
        if let detached {
            guard detached.sameObject(header.version) else { throw Failure.changed }
            name = header.tombstone
        } else if let original {
            guard original.sameObject(header.version) else { throw Failure.changed }
            try requireDisabled(owner, header: header, checkRevision: true)
            name = header.releaseID
        } else {
            // Observation after an interruption is not proof that we deleted it.
            guard synchronize(root) == 0 else { return Result(releaseID: header.releaseID, removed: 0, observedAbsent: members.count + 1, complete: false, durabilityConfirmed: false) }
            try clearIntent(root, expected: header, members: members)
            return Result(releaseID: header.releaseID, removed: 0, observedAbsent: members.count + 1, complete: true, durabilityConfirmed: synchronize(root) == 0)
        }
        let fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw Failure.changed }
        let handle = PluginReleaseCatalog.VersionHandle(fd, id: header.releaseID)
        guard try handle.exclusiveUse() else { throw Failure.occupied }
        guard header.version.sameObject(try statFD(fd)), try namedStamp(root, name)?.sameObject(header.version) == true else { throw Failure.changed }
        let removal = PluginReleaseRemoval(root: root, version: handle, directory: owner.url.appendingPathComponent(name),
            header: header, members: members, synchronize: synchronize, checkpoint: checkpoint)
        transferred = true
        return try removal.finish(owner: owner, alreadyDetached: name == header.tombstone)
    }
    private func finish(owner: PluginReleaseCatalog, alreadyDetached: Bool) throws -> Result {
        // Acquire the current root lock in the caller. Preserve enabled choices
        // and replacements even when the old detached directory still exists.
        try Self.requireDisabled(owner, header: header, checkRevision: !alreadyDetached)
        let current = try Self.scan(version.fd, device: header.root.device)
        let expected = Dictionary(uniqueKeysWithValues: members.map { ($0.path, $0) })
        for item in current {
            guard let old = expected[item.path], item.directory == old.directory,
                  item.directory ? item.stamp.sameObject(old.stamp) : item == old else { throw Failure.changed }
        }
        let absent = members.count - current.count
        if !alreadyDetached {
            guard try Self.namedStamp(root, header.releaseID)?.sameObject(header.version) == true else { throw Failure.changed }
            guard renameatx_np(root, header.releaseID, root, header.tombstone, UInt32(RENAME_EXCL)) == 0 else { throw Failure.io }
        }
        guard synchronize(root) == 0 else { return result(0, absent, false, false) }
        try checkpoint("after-detach")
        var removed = 0
        for member in current.sorted(by: { $0.path.split(separator: "/").count > $1.path.split(separator: "/").count }) {
            let components = member.path.split(separator: "/").map(String.init)
            let parent = try Self.parentFD(version.fd, components: Array(components.dropLast()), expected: expected)
            defer { Darwin.close(parent) }
            let name = components.last!
            guard let now = try Self.namedStamp(parent, name),
                  member.directory ? now.sameObject(member.stamp) : now == member.stamp else { throw Failure.changed }
            if !member.directory {
                let bytes = try PluginReleaseCatalog.read(parent, name: name, limit: 16777216)
                guard PluginReleaseCatalog.sha(bytes) == member.sha256,
                      try Self.namedStamp(parent, name) == member.stamp else { throw Failure.changed }
            }
            guard fchmod(parent, 0o700) == 0,
                  unlinkat(parent, name, member.directory ? AT_REMOVEDIR : 0) == 0 else { throw Failure.io }
            removed += 1
            try checkpoint("after-unlink")
        }
        guard try Self.namedStamp(root, header.tombstone)?.sameObject(header.version) == true,
              unlinkat(root, header.tombstone, AT_REMOVEDIR) == 0 else { throw Failure.changed }
        removed += 1
        guard synchronize(root) == 0 else { return result(removed, absent, false, false) }
        try checkpoint("before-clear")
        try Self.clearIntent(root, expected: header, members: members)
        return result(removed, absent, true, synchronize(root) == 0)
    }
    private func result(_ removed: Int, _ absent: Int, _ complete: Bool, _ durable: Bool) -> Result {
        Result(releaseID: header.releaseID, removed: removed, observedAbsent: absent, complete: complete, durabilityConfirmed: durable)
    }
    private static func requireDisabled(_ owner: PluginReleaseCatalog, header: Header, checkRevision: Bool) throws {
        let choice = try owner.selection()
        guard !checkRevision || choice.revision == header.revision else { throw PluginReleaseCatalog.Failure.conflict }
        guard !choice.choices.values.contains(where: { $0.releaseID == header.releaseID && $0.enabled }) else { throw Failure.enabled }
    }
    private static func statFD(_ fd: Int32) throws -> Stamp {
        var s = stat()
        guard fstat(fd, &s) == 0, s.st_uid == geteuid() else { throw Failure.invalid }
        return Stamp(s)
    }
    private static func namedStamp(_ parent: Int32, _ name: String) throws -> Stamp? {
        var s = stat()
        if fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW) != 0 {
            guard errno == ENOENT else { throw Failure.io }; return nil
        }
        guard s.st_uid == geteuid() else { throw Failure.invalid }; return Stamp(s)
    }
    private static func scan(_ fd: Int32, device: String) throws -> [Member] {
        var result: [Member] = [], bytes: Int64 = 0
        func visit(_ parent: Int32, prefix: String, depth: Int) throws {
            guard depth <= 34 else { throw Failure.invalid }
            let copy = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            guard copy >= 0 else { throw Failure.io }
            guard let stream = fdopendir(copy) else { Darwin.close(copy); throw Failure.io }
            defer { closedir(stream) }
            while true {
                errno = 0
                guard let entry = readdir(stream) else { guard errno == 0 else { throw Failure.io }; break }
                let name = withUnsafePointer(to: &entry.pointee.d_name) { $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) } }
                if name == "." || name == ".." { continue }
                let path = prefix.isEmpty ? name : prefix + "/" + name
                guard validPath(path), result.count < 4096, let stamp = try namedStamp(parent, name),
                      stamp.device == device, let mode = UInt16(stamp.mode), let size = Int64(stamp.size), size >= 0 else { throw Failure.invalid }
                let directory = mode & S_IFMT == S_IFDIR
                guard directory || (mode & S_IFMT == S_IFREG && stamp.links == "1" && size <= 16777216) else { throw Failure.invalid }
                var hash: String?
                if directory {
                    result.append(Member(path: path, directory: true, stamp: stamp, sha256: nil))
                    let child = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                    guard child >= 0 else { throw Failure.changed }
                    defer { Darwin.close(child) }
                    guard try statFD(child) == stamp else { throw Failure.changed }
                    try visit(child, prefix: path, depth: depth + 1)
                } else {
                    bytes += size; guard bytes <= 68157440 else { throw Failure.invalid }
                    hash = PluginReleaseCatalog.sha(try PluginReleaseCatalog.read(parent, name: name, limit: 16777216))
                    result.append(Member(path: path, directory: false, stamp: stamp, sha256: hash))
                }
                guard try namedStamp(parent, name) == stamp else { throw Failure.changed }
            }
        }
        try visit(fd, prefix: "", depth: 0)
        return result.sorted { $0.path < $1.path }
    }
    private static func validPath(_ path: String) -> Bool {
        !path.isEmpty && path.utf8.count <= 4096 && !path.utf8.contains(0) && !path.contains("\\") &&
            path.split(separator: "/", omittingEmptySubsequences: false).allSatisfy { !$0.isEmpty && $0 != "." && $0 != ".." && $0.utf8.count <= 255 }
    }
    private static func parentFD(_ root: Int32, components: [String], expected: [String: Member]) throws -> Int32 {
        var current = fcntl(root, F_DUPFD_CLOEXEC, 3)
        guard current >= 0 else { throw Failure.io }
        do {
            var prefix = ""
            for name in components {
                prefix = prefix.isEmpty ? name : prefix + "/" + name
                guard let member = expected[prefix], member.directory else { throw Failure.invalid }
                let next = openat(current, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard next >= 0 else { throw Failure.changed }
                let stamp = try? statFD(next)
                guard stamp.map(member.stamp.sameObject) == true else { Darwin.close(next); throw Failure.changed }
                Darwin.close(current); current = next
            }
            return current
        } catch { Darwin.close(current); throw error }
    }
    private static func writeIntent(root: Int32, header: Header, members: [Member]) throws {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        var bytes = try encoder.encode(header); bytes.append(10)
        for member in members {
            let line = try encoder.encode(member)
            guard line.count <= 16384 else { throw Failure.invalid }
            bytes.append(line); bytes.append(10)
        }
        guard bytes.count <= 8388608 else { throw Failure.invalid }
        let name = ".removal-record-" + UUID().uuidString
        let fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard fd >= 0 else { throw Failure.io }
        var published = false
        defer { Darwin.close(fd); if !published { _ = unlinkat(root, name, 0) } }
        try bytes.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let written = Darwin.write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if written < 0 && errno == EINTR { continue }
                guard written > 0 else { throw Failure.io }; offset += written
            }
        }
        guard fsync(fd) == 0,
              renameatx_np(root, name, root, PluginReleaseCatalog.removalName, UInt32(RENAME_EXCL)) == 0 else { throw Failure.io }
        published = true
    }
    private static func readIntent(_ root: Int32) throws -> (Header, [Member], Stamp) {
        guard let before = try namedStamp(root, PluginReleaseCatalog.removalName), before.links == "1",
              UInt16(before.mode).map({ $0 & 0o077 == 0 }) == true else { throw Failure.invalid }
        let raw = try PluginReleaseCatalog.read(root, name: PluginReleaseCatalog.removalName, limit: 8388608)
        guard try namedStamp(root, PluginReleaseCatalog.removalName) == before, raw.last == 10 else { throw Failure.changed }
        let lines = raw.split(separator: 10, omittingEmptySubsequences: false)
        guard lines.count >= 2, lines.count <= 4098, lines.last?.isEmpty == true else { throw Failure.invalid }
        let decoder = JSONDecoder()
        func decode<T: Decodable>(_ type: T.Type, _ line: Data.SubSequence, keys: Set<String>, optional: String) throws -> T {
            guard line.count <= 16384 else { throw Failure.invalid }
            let object = try ReleaseJSON.parse(Data(line)).object(), actual = Set(object.keys)
            guard actual == keys || actual == keys.union([optional]) else { throw Failure.invalid }
            let stampKeys: Set<String> = ["device", "inode", "uid", "mode", "links", "size", "modified", "changed", "born"]
            for key in ["root", "version", "stamp"] where object[key] != nil {
                guard Set(try object[key]!.object().keys) == stampKeys else { throw Failure.invalid }
            }
            return try decoder.decode(type, from: Data(line))
        }
        let header = try decode(Header.self, lines[0], keys: ["schema", "operation", "releaseID", "tombstone", "root", "version"], optional: "revision")
        guard header.schema == 1, UUID(uuidString: header.operation) != nil,
              PluginReleaseCatalog.digest(header.releaseID), header.tombstone == ".removing-" + header.operation,
              header.revision == nil || PluginReleaseCatalog.digest(header.revision!) else { throw Failure.invalid }
        var members: [Member] = [], paths = Set<String>(), bytes: Int64 = 0
        for line in lines.dropFirst().dropLast() {
            let member = try decode(Member.self, line, keys: ["path", "directory", "stamp"], optional: "sha256")
            guard validPath(member.path), paths.insert(member.path).inserted,
                  member.directory ? member.sha256 == nil : member.sha256.map(PluginReleaseCatalog.digest) == true,
                  let size = Int64(member.stamp.size), size >= 0, size <= (member.directory ? Int64.max : 16777216) else { throw Failure.invalid }
            if !member.directory { bytes += size; guard bytes <= 68157440 else { throw Failure.invalid } }
            members.append(member)
        }
        let byPath = Dictionary(uniqueKeysWithValues: members.map { ($0.path, $0) })
        for member in members {
            let components = member.path.split(separator: "/").dropLast()
            var path = ""
            for component in components {
                path = path.isEmpty ? String(component) : path + "/" + component
                guard byPath[path]?.directory == true else { throw Failure.invalid }
            }
        }
        return (header, members, before)
    }
    private static func clearIntent(_ root: Int32, expected: Header, members: [Member]) throws {
        let (current, currentMembers, stamp) = try readIntent(root)
        guard current == expected, currentMembers == members, try namedStamp(root, PluginReleaseCatalog.removalName) == stamp,
              unlinkat(root, PluginReleaseCatalog.removalName, 0) == 0 else { throw Failure.changed }
    }
}

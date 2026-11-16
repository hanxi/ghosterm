import Darwin
import Foundation

/// Durable installation namespace reservations. A reservation without a matching
/// directory identity is unknown, never authority to adopt a same-named object.
final class PluginInstallJournal {
    static let directoryName = "install-journal-v1"
    struct Identity: Codable, Equatable, Sendable {
        let device, inode, owner, born: String
        init(_ s: stat) {
            device = String(s.st_dev); inode = String(s.st_ino); owner = String(s.st_uid)
            born = "\(s.st_birthtimespec.tv_sec):\(s.st_birthtimespec.tv_nsec)"
        }
    }
    struct Reference: Codable, Equatable, Sendable {
        let id, name, digest: String
    }
    struct Record: Codable, Equatable {
        let schema: Int
        let id, name: String
        let parent: Identity
        var directory: Identity?
        var target: String?
    }
    enum State: String, Sendable { case active, abandoned, published, absent, unconfirmed, changed }
    struct Entry: Sendable, Equatable {
        let reference: Reference
        let state: State
        let target: String?
    }
    struct Listing { let entries: [Entry]; let issues: [String] }
    private let parent: Int32, fd: Int32
    private let parentIdentity: Identity
    private var retired: [String: Reference] = [:]
    init?(parent: Int32, create: Bool = false) throws {
        var value = stat()
        guard fstat(parent, &value) == 0, value.st_uid == geteuid(), value.st_mode & 0o077 == 0 else { throw PluginReleaseStage.Failure.root }
        parentIdentity = Identity(value); self.parent = parent
        if create {
            if mkdirat(parent, Self.directoryName, 0o700) != 0 && errno != EEXIST { throw PluginReleaseStage.Failure.io }
        }
        let opened = openat(parent, Self.directoryName, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        if opened < 0 && errno == ENOENT && !create { return nil }
        guard opened >= 0 else { throw PluginReleaseStage.Failure.changed }
        guard fstat(opened, &value) == 0, value.st_uid == geteuid(), value.st_mode & 0o077 == 0,
              value.st_dev == dev_t(parentIdentity.device) else { Darwin.close(opened); throw PluginReleaseStage.Failure.changed }
        if create, fsync(parent) != 0 { Darwin.close(opened); throw PluginReleaseStage.Failure.io }
        fd = opened
    }
    deinit { Darwin.close(fd) }
    private func validate() throws {
        var expected = stat(), current = stat()
        guard fstat(fd, &expected) == 0, fstatat(parent, Self.directoryName, &current, AT_SYMLINK_NOFOLLOW) == 0,
              Identity(expected) == Identity(current), current.st_mode & S_IFMT == S_IFDIR,
              current.st_uid == geteuid(), current.st_mode & 0o077 == 0,
              fstat(parent, &current) == 0, Identity(current) == parentIdentity,
              current.st_mode & 0o077 == 0 else { throw PluginReleaseStage.Failure.changed }
    }
    func reserve(id: String) throws -> Record {
        let existing = try listing()
        guard existing.entries.count + existing.issues.count < 64 else { throw PluginReleaseStage.Failure.limit }
        let record = Record(schema: 1, id: id, name: ".prepare-" + id.lowercased(), parent: parentIdentity)
        try write(record, expected: nil)
        return record
    }
    func write(_ record: Record, expected: String?) throws {
        try validate()
        guard UUID(uuidString: record.id) != nil, record.name == ".prepare-" + record.id.lowercased(), record.parent == parentIdentity else { throw PluginReleaseStage.Failure.changed }
        let filename = record.id + ".json"
        var value = stat()
        if fstatat(fd, filename, &value, AT_SYMLINK_NOFOLLOW) == 0 {
            guard let expected, try read(record.id).1.digest == expected else { throw PluginReleaseStage.Failure.changed }
        } else { guard errno == ENOENT, expected == nil else { throw PluginReleaseStage.Failure.changed } }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let raw = try encoder.encode(record)
        let temporary = ".write-" + UUID().uuidString
        let file = openat(fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard file >= 0 else { throw PluginReleaseStage.Failure.io }
        var published = false
        defer { Darwin.close(file); if !published { _ = unlinkat(fd, temporary, 0) } }
        try raw.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let count = Darwin.write(file, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw PluginReleaseStage.Failure.io }; offset += count
            }
        }
        guard fsync(file) == 0 else { throw PluginReleaseStage.Failure.io }
        try validate()
        if let expected { guard try read(record.id).1.digest == expected else { throw PluginReleaseStage.Failure.changed } }
        let flags: UInt32 = expected == nil ? UInt32(RENAME_EXCL) : 0
        guard renameatx_np(fd, temporary, fd, filename, flags) == 0 else { throw PluginReleaseStage.Failure.io }
        published = true
        guard fsync(fd) == 0 else { throw PluginReleaseStage.Failure.io }
    }
    func read(_ id: String) throws -> (Record, Reference) {
        guard UUID(uuidString: id) != nil else { throw PluginReleaseStage.Failure.changed }
        try validate()
        var value = stat()
        guard fstatat(fd, id + ".json", &value, AT_SYMLINK_NOFOLLOW) == 0, value.st_nlink == 1,
              value.st_mode & 0o077 == 0 else { throw PluginReleaseStage.Failure.changed }
        let raw = try PluginReleaseCatalog.read(fd, name: id + ".json", limit: 16384)
        let object = try ReleaseJSON.parse(raw).object()
        let required: Set<String> = ["schema", "id", "name", "parent"]
        guard Set(object.keys).subtracting(["directory", "target"]) == required else { throw PluginReleaseStage.Failure.changed }
        for key in ["parent", "directory"] where object[key] != nil {
            guard Set(try object[key]!.object().keys) == ["device", "inode", "owner", "born"] else { throw PluginReleaseStage.Failure.changed }
        }
        let record = try JSONDecoder().decode(Record.self, from: raw)
        guard record.schema == 1, record.id == id, record.name == ".prepare-" + id.lowercased(), record.parent == parentIdentity,
              record.target == nil || PluginReleaseCatalog.digest(record.target!) else { throw PluginReleaseStage.Failure.changed }
        return (record, Reference(id: id, name: record.name, digest: PluginReleaseCatalog.sha(raw)))
    }
    private func identity(_ name: String) throws -> Identity? {
        var value = stat()
        if fstatat(parent, name, &value, AT_SYMLINK_NOFOLLOW) != 0 {
            guard errno == ENOENT else { throw PluginReleaseStage.Failure.io }; return nil
        }
        guard value.st_mode & S_IFMT == S_IFDIR, value.st_uid == geteuid(), String(value.st_dev) == parentIdentity.device else { throw PluginReleaseStage.Failure.changed }
        return Identity(value)
    }
    func entry(_ id: String) throws -> Entry {
        let (record, reference) = try read(id)
        if let target = record.target, let expected = record.directory, (try? identity(target)) == expected {
            return Entry(reference: reference, state: .published, target: target)
        }
        guard let actual = try identity(record.name) else { return Entry(reference: reference, state: .absent, target: record.target) }
        guard let expected = record.directory else { return Entry(reference: reference, state: .unconfirmed, target: record.target) }
        guard actual == expected else { return Entry(reference: reference, state: .changed, target: record.target) }
        let file = openat(parent, record.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard file >= 0 else { throw PluginReleaseStage.Failure.changed }
        defer { Darwin.close(file) }
        var value = stat()
        guard fstat(file, &value) == 0, Identity(value) == expected else { throw PluginReleaseStage.Failure.changed }
        let free = flock(file, LOCK_EX | LOCK_NB) == 0
        guard free || errno == EWOULDBLOCK else { throw PluginReleaseStage.Failure.io }
        return Entry(reference: reference, state: free ? .abandoned : .active, target: record.target)
    }
    func openAbandoned(id: String, expected: String) throws -> (Reference, PluginReleaseCatalog.VersionHandle) {
        let entry = try entry(id)
        guard entry.reference.digest == expected, entry.state == .abandoned else { throw PluginReleaseStage.Failure.changed }
        let (record, reference) = try read(id)
        let file = openat(parent, record.name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard file >= 0 else { throw PluginReleaseStage.Failure.changed }
        let handle = PluginReleaseCatalog.VersionHandle(file, id: reference.digest)
        guard try handle.exclusiveUse() else { throw PluginReleaseStage.Failure.busy }
        var value = stat()
        guard fstat(file, &value) == 0, Identity(value) == record.directory else { throw PluginReleaseStage.Failure.changed }
        return (reference, handle)
    }
    func verify(_ reference: Reference) throws {
        guard try read(reference.id).1 == reference else { throw PluginReleaseStage.Failure.changed }
    }
    func retire(_ reference: Reference, requireResolved: Bool = true) throws {
        if retired[reference.id] == reference {
            var current = stat()
            guard fstatat(fd, reference.id + ".json", &current, AT_SYMLINK_NOFOLLOW) != 0, errno == ENOENT,
                  fsync(fd) == 0 else { throw PluginReleaseStage.Failure.changed }
            return
        }
        try verify(reference)
        if requireResolved {
            let state = try entry(reference.id).state
            guard state == .absent || state == .published else { throw PluginReleaseStage.Failure.changed }
        }
        try verify(reference)
        guard unlinkat(fd, reference.id + ".json", 0) == 0 else { throw PluginReleaseStage.Failure.io }
        retired[reference.id] = reference
        guard fsync(fd) == 0 else { throw PluginReleaseStage.Failure.io }
    }
    func listing() throws -> Listing {
        try validate()
        let copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard copy >= 0, let stream = fdopendir(copy) else { if copy >= 0 { Darwin.close(copy) }; throw PluginReleaseStage.Failure.io }
        defer { closedir(stream) }
        var entries: [Entry] = [], issues: [String] = [], count = 0
        while true {
            errno = 0
            guard let item = readdir(stream) else { guard errno == 0 else { throw PluginReleaseStage.Failure.io }; break }
            let name = withUnsafePointer(to: &item.pointee.d_name) { $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) } }
            if name == "." || name == ".." { continue }
            count += 1; guard count <= 128 else { throw PluginReleaseStage.Failure.limit }
            guard name.hasSuffix(".json"), UUID(uuidString: String(name.dropLast(5))) != nil else { issues.append("未采纳的安装元数据：\(name)"); continue }
            do { entries.append(try entry(String(name.dropLast(5)))) }
            catch { issues.append("\(name)：\(error)") }
        }
        return Listing(entries: entries.sorted { $0.reference.id < $1.reference.id }, issues: issues)
    }
}

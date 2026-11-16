import Darwin
import Foundation

/// Observations only. Being listed here does not establish installation origin.
enum PluginUnclassifiedDirectories {
    struct Entry: Sendable, Equatable {
        let name, identity: String
        let canPreview: Bool
        let issue: String?
    }
    static func allowedName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 255 && !name.utf8.contains(0) && !name.contains("/") && !name.contains("\\") &&
            ![".", "..", PluginReleaseCatalog.selectionName, PluginReleaseCatalog.removalName, PluginInstallJournal.directoryName].contains(name)
    }
    static func token(parent: Int32, name: String, value: stat) throws -> String {
        var root = stat()
        guard fstat(parent, &root) == 0 else { throw PluginReleaseCatalog.Failure.io }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let encoded = try encoder.encode(PluginReleaseRemoval.Stamp(value))
        var bytes = Data("\(root.st_dev):\(root.st_ino):\(root.st_uid):\(root.st_birthtimespec.tv_sec):\(root.st_birthtimespec.tv_nsec):\(name)\0".utf8)
        bytes.append(encoded)
        return PluginReleaseCatalog.sha(bytes)
    }
    static func open(owner: PluginReleaseCatalog, name: String, expected: String) throws -> PluginReleaseCatalog.VersionHandle {
        guard allowedName(name), PluginReleaseCatalog.digest(expected) else { throw PluginReleaseRemoval.Failure.invalid }
        let root = try owner.removalRoot(); defer { Darwin.close(root) }
        let file = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard file >= 0 else { throw PluginReleaseRemoval.Failure.invalid }
        let handle = PluginReleaseCatalog.VersionHandle(file, id: PluginReleaseCatalog.sha(Data(name.utf8)))
        var value = stat(), parent = stat(), named = stat()
        guard fstat(file, &value) == 0, fstat(root, &parent) == 0, value.st_uid == geteuid(), value.st_dev == parent.st_dev,
              fstatat(root, name, &named, AT_SYMLINK_NOFOLLOW) == 0, PluginReleaseRemoval.Stamp(named) == PluginReleaseRemoval.Stamp(value),
              try token(parent: root, name: name, value: value) == expected else { throw PluginReleaseRemoval.Failure.changed }
        return handle
    }
    static func listing(owner: PluginReleaseCatalog) throws -> [Entry] {
        let root = try owner.journalRoot(); defer { Darwin.close(root) }
        var known = Set(try owner.inventory().versions.map(\.releaseID))
        if let journal = try PluginInstallJournal(parent: root) {
            known.formUnion(try journal.listing().entries.filter { $0.state == .active || $0.state == .abandoned }.map { $0.reference.name })
        }
        let copy = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard copy >= 0, let stream = fdopendir(copy) else { if copy >= 0 { Darwin.close(copy) }; throw PluginReleaseCatalog.Failure.io }
        defer { closedir(stream) }
        var entries: [Entry] = [], count = 0, parent = stat()
        guard fstat(root, &parent) == 0 else { throw PluginReleaseCatalog.Failure.io }
        while true {
            errno = 0
            guard let item = readdir(stream) else { guard errno == 0 else { throw PluginReleaseCatalog.Failure.io }; break }
            let name = withUnsafePointer(to: &item.pointee.d_name) { $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) } }
            guard allowedName(name) else { continue }
            count += 1; guard count <= 128 else { throw PluginReleaseCatalog.Failure.invalid }
            if known.contains(name) { continue }
            var value = stat()
            guard fstatat(root, name, &value, AT_SYMLINK_NOFOLLOW) == 0 else { throw PluginReleaseCatalog.Failure.changed }
            let supported = value.st_mode & S_IFMT == S_IFDIR && value.st_uid == geteuid() && value.st_dev == parent.st_dev
            entries.append(Entry(name: name, identity: try token(parent: root, name: name, value: value), canPreview: supported,
                issue: supported ? nil : "类型、设备或归属不满足目录预览条件，保留此条目。"))
        }
        return entries.sorted { $0.name < $1.name }
    }
}

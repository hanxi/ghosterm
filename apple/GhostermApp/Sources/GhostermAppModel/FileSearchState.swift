import Foundation

enum FileSearchError: Error, CustomStringConvertible {
    case cancelled, incomplete(String)
    var description: String {
        switch self {
        case .cancelled: return "搜索已取消"
        case .incomplete(let message): return message
        }
    }
}

struct FileSearchLimits {
    var results = 5000
    var scanned = 100_000
    var metadataBytes = 8 * 1024 * 1024
    var depth = 64
}

struct FileSearchSnapshot {
    let entries: [FileEntry]
    let scanned: Int
    let stopped: String?
}

/// Callbacks may finish on a backend thread. Publish immutable value snapshots.
final class FileSearchState: @unchecked Sendable {
    private let lock = NSLock()
    private let limits: FileSearchLimits
    private var entries: [FileEntry] = []
    private var seen: Set<Data> = []
    private var scanned = 0
    private var metadataBytes = 0
    private var stopped: String?
    private var publishedScanned = -100
    private var publishedHits = 0
    init(limits: FileSearchLimits) { self.limits = limits }
    func snapshot() -> FileSearchSnapshot {
        lock.lock(); defer { lock.unlock() }
        return FileSearchSnapshot(entries: entries, scanned: scanned, stopped: stopped)
    }
    func progress() -> FileSearchSnapshot? {
        lock.lock(); defer { lock.unlock() }
        guard scanned - publishedScanned >= 100 || (publishedHits == 0 && !entries.isEmpty) else { return nil }
        publishedScanned = scanned; publishedHits = entries.count
        return FileSearchSnapshot(entries: entries, scanned: scanned, stopped: stopped)
    }
    func scan(_ value: Int? = nil) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard stopped == nil else { return false }
        scanned = max(scanned, value ?? (scanned + 1))
        if scanned > limits.scanned { stopped = "达到搜索扫描上限，请缩小目录后重试" }
        return stopped == nil
    }
    func append(_ entry: FileEntry) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard stopped == nil else { return false }
        let key = Data(entry.path.utf8)
        if seen.contains(key) { return true }
        let pathBytes = entry.path.utf8.count, nameBytes = entry.name.utf8.count
        guard entries.count < limits.results, pathBytes <= 65536, nameBytes <= 65536,
              pathBytes + nameBytes <= limits.metadataBytes - metadataBytes else {
            stopped = "达到搜索结果或元数据上限，请缩小目录后重试"
            return false
        }
        entries.append(entry); seen.insert(key); metadataBytes += pathBytes + nameBytes
        return true
    }
}

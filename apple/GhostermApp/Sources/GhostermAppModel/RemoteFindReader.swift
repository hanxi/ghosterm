import Foundation
import GhostermCore

/// NUL framed paths from an owned exec channel. Metadata errors and an unknown
/// exit status leave a partial result; neither is evidence of a complete search.
enum RemoteFindReader {
    static func run(root: String, pattern: String,
                    isCancelled: () -> Bool,
                    read: () throws -> SSHExec.ReadResult,
                    exitStatus: () -> Int32?,
                    lookup: (String) throws -> FileEntry,
                    onHit: (FileEntry) -> Bool, onScanned: (Int) -> Void,
                    clock: () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
                    outputLimit: Int = 16 * 1024 * 1024,
                    recordLimit: Int = 65536) throws {
        var pending = Data(), total = 0, scanned = 0, stderrSeen = false
        let deadline = clock() + 60
        let rootBytes = Data(root.utf8)
        let prefix = root == "/" ? rootBytes : rootBytes + Data([47])
        func check() throws {
            if isCancelled() { throw FileSearchError.cancelled }
            if clock() >= deadline { throw FileSearchError.incomplete("远端搜索超过 60 秒，请缩小目录后重试") }
        }
        while true {
            try check()
            let result = try read()
            try check()
            switch result {
            case .data(let bytes, let isStderr):
                guard bytes.count <= outputLimit - total else { throw FileSearchError.incomplete("远端搜索输出超过上限") }
                total += bytes.count
                if isStderr { stderrSeen = stderrSeen || !bytes.isEmpty; continue }
                pending.append(bytes)
                while let nul = pending.firstIndex(of: 0) {
                    try check()
                    let record = pending.prefix(upTo: nul)
                    guard !record.isEmpty, record.count <= recordLimit,
                          let path = String(data: record, encoding: .utf8),
                          record.starts(with: prefix), !path.split(separator: "/").contains("..") else {
                        throw FileSearchError.incomplete("远端搜索返回了无效或越界的路径")
                    }
                    pending.removeSubrange(pending.startIndex...nul)
                    scanned += 1
                    onScanned(scanned)
                    try check()
                    if SFTPFileBackend.matches((path as NSString).lastPathComponent, pattern) {
                        let entry = try lookup(path)
                        try check()
                        if !onHit(entry) { throw FileSearchError.cancelled }
                    }
                }
                guard pending.count <= recordLimit else { throw FileSearchError.incomplete("远端搜索路径超过上限") }
            case .timeout: continue
            case .eof:
                guard pending.isEmpty else { throw FileSearchError.incomplete("远端搜索路径记录不完整") }
                guard let status = exitStatus() else { throw FileSearchError.incomplete("远端搜索退出状态未知") }
                guard status == 0, !stderrSeen else { throw FileSearchError.incomplete("远端搜索命令未完整成功（退出码 \(status)）") }
                return
            }
        }
    }
}

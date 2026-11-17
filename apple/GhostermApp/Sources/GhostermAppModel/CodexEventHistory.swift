import Foundation

/// Recent diagnostic payloads, independent of the accepted sequence high-water
/// mark, pending approvals, and complete file-review snapshots.
enum CodexEventHistory {
    static let maximumEvents = 128
    static let maximumBytes = 262144
    static let maximumEventBytes = 3072
    private static let kinds = Set(["notification", "approval-request", "approval-resolved",
                                    "outcome-unknown", "protocol-error", "closed"])

    static func appending(_ encoded: String, kind: String, sequence: UInt64,
                          after previousSequence: UInt64, to history: [String]) -> [String]? {
        guard previousSequence < UInt64.max, sequence == previousSequence + 1,
              kinds.contains(kind), encoded.utf8.count <= maximumEventBytes else { return nil }
        var recent = history
        recent.append(encoded)
        var bytes = recent.reduce(0) { $0 + $1.utf8.count }
        var removed = 0
        while recent.count - removed > maximumEvents || bytes > maximumBytes {
            bytes -= recent[removed].utf8.count
            removed += 1
        }
        if removed != 0 { recent.removeFirst(removed) }
        return recent
    }
}

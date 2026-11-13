import Foundation

public struct CodexDiffLine: Identifiable, Sendable, Equatable {
    public enum Kind: Sendable, Equatable { case context, added, removed, hunk }
    public let id: Int
    public let text: String
    public let kind: Kind
}

public struct CodexFileChange: Identifiable, Sendable, Equatable {
    public let path: String
    public let kind: String
    public let movePath: String?
    public let lines: [CodexDiffLine]
    public let diffTruncated: Bool
    public var id: String { path }
}

public struct CodexTimelineEntry: Identifiable, Sendable, Equatable {
    public let id: String
    public let title: String
    public let detail: String
    public let fileChanges: [CodexFileChange]
    public let omittedFileCount: Int
    public let changeReference: CodexChangeReference?
    public let changeUnavailable: Bool

    public init(id: String, title: String, detail: String, fileChanges: [CodexFileChange] = [],
                omittedFileCount: Int = 0, changeReference: CodexChangeReference? = nil,
                changeUnavailable: Bool = false) {
        self.id = id
        self.title = title
        self.detail = detail
        self.fileChanges = fileChanges
        self.omittedFileCount = omittedFileCount
        self.changeReference = changeReference
        self.changeUnavailable = changeUnavailable
    }
}

enum CodexTimelineReducer {
    private static let maximumEntries = 128
    private static let maximumEntryBytes = 32768
    private static let maximumTimelineBytes = 262144

    static func apply(_ entries: [CodexTimelineEntry], sequence: UInt64,
                      method: String, params: Data) -> [CodexTimelineEntry] {
        guard let value = try? JSONSerialization.jsonObject(with: params) as? [String: Any] else { return entries }
        if method == "serverRequest/resolved" { return entries }
        if method == "protocol-error" {
            return replacing(entries, with: CodexTimelineEntry(id: "event:\(sequence)", title: "Codex 协议错误",
                detail: value["message"] as? String ?? json(value)))
        }
        if method == "outcome-unknown" {
            return replacing(entries, with: CodexTimelineEntry(id: "event:\(sequence)", title: "执行结果未知",
                detail: json(value)))
        }
        if method == "closed" {
            return replacing(entries, with: CodexTimelineEntry(id: "event:\(sequence)", title: "Codex 会话已关闭",
                detail: value["outcome"] as? String ?? ""))
        }

        let item = value["item"] as? [String: Any]
        if method == "item/started" || method == "item/completed", let item,
           let itemID = item["id"] as? String, let type = item["type"] as? String {
            if type == "fileChange" {
                let marker = item["_ghosterm"] as? [String: Any]
                return replacing(entries, with: CodexTimelineEntry(id: "item:\(itemID)", title: "文件变更",
                    detail: "", fileChanges: fileChanges(item["changes"], marker: marker),
                    omittedFileCount: max(0, marker?["omittedChanges"] as? Int ?? 0) +
                        max(0, ((item["changes"] as? [Any])?.count ?? 0) - 8),
                    changeReference: CodexChangeReference.decode(marker: marker, thread: value["threadId"],
                        turn: value["turnId"], item: itemID), changeUnavailable: marker?["snapshotUnavailable"] != nil))
            }
            let (title, detail) = itemPresentation(item, type: type, previous: entries.first { $0.id == "item:\(itemID)" }?.detail)
            return replacing(entries, with: CodexTimelineEntry(id: "item:\(itemID)", title: title, detail: detail))
        }

        if method == "item/agentMessage/delta" || method == "item/commandExecution/outputDelta",
           let itemID = value["itemId"] as? String, let delta = value["delta"] as? String {
            let id = "item:\(itemID)"
            let previous = entries.first { $0.id == id }
            let title = previous?.title ?? (method == "item/agentMessage/delta" ? "Codex" : "命令输出")
            let detail: String
            if method == "item/commandExecution/outputDelta" {
                let old = previous?.detail ?? ""
                detail = old.contains("\n输出：\n") ? old + delta : old + (old.isEmpty ? "输出：\n" : "\n输出：\n") + delta
            } else {
                detail = (previous?.detail ?? "") + delta
            }
            return replacing(entries, with: CodexTimelineEntry(id: id, title: title, detail: bounded(detail)))
        }

        if method == "item/fileChange/patchUpdated", let itemID = value["itemId"] as? String {
            let marker = value["_ghosterm"] as? [String: Any]
            return replacing(entries, with: CodexTimelineEntry(id: "item:\(itemID)", title: "文件变更",
                detail: "", fileChanges: fileChanges(value["changes"], marker: marker),
                omittedFileCount: max(0, marker?["omittedChanges"] as? Int ?? 0) +
                    max(0, ((value["changes"] as? [Any])?.count ?? 0) - 8),
                changeReference: CodexChangeReference.decode(marker: marker, thread: value["threadId"],
                    turn: value["turnId"], item: itemID), changeUnavailable: marker?["snapshotUnavailable"] != nil))
        }

        let title: String
        let detail: String
        if method == "turn/started" {
            title = "Turn 已启动"
            detail = nestedStatus(value)
        } else if method == "turn/completed" {
            title = "Turn 已完成"
            detail = nestedStatus(value)
        } else {
            title = method
            detail = json(value)
        }
        return replacing(entries, with: CodexTimelineEntry(id: "event:\(sequence)", title: title, detail: detail))
    }

    private static func fileChanges(_ value: Any?, marker: Any?) -> [CodexFileChange] {
        let truncated = (marker as? [String: Any])?["diffTruncated"] as? Bool == true
        guard let changes = value as? [[String: Any]] else { return [] }
        return changes.prefix(8).compactMap { change in
            guard let path = change["path"] as? String, !path.isEmpty, path.utf8.count <= 512,
                  let kindObject = change["kind"] as? [String: Any],
                  let kind = kindObject["type"] as? String, ["add", "delete", "update"].contains(kind),
                  let diff = change["diff"] as? String else { return nil }
            let movePath = kind == "update" ? kindObject["move_path"] as? String : nil
            var used = 0
            let boundedDiff = String(diff.prefix(while: { character in
                let count = String(character).utf8.count
                guard used + count <= 32768 else { return false }
                used += count
                return true
            }))
            let allLines = boundedDiff.split(separator: "\n", omittingEmptySubsequences: false)
            let lines = allLines.prefix(256).enumerated().map { index, line -> CodexDiffLine in
                let text = String(line)
                let lineKind: CodexDiffLine.Kind
                if text.hasPrefix("@@") { lineKind = .hunk }
                else if text.hasPrefix("+") { lineKind = .added }
                else if text.hasPrefix("-") { lineKind = .removed }
                else { lineKind = .context }
                return CodexDiffLine(id: index, text: text, kind: lineKind)
            }
            return CodexFileChange(path: path, kind: kind, movePath: movePath, lines: lines,
                diffTruncated: truncated || used < diff.utf8.count || allLines.count > lines.count ||
                    diff.contains("…（diff 已截断）"))
        }
    }

    private static func itemPresentation(_ item: [String: Any], type: String,
                                         previous: String?) -> (String, String) {
        switch type {
        case "agentMessage":
            return ("Codex", item["text"] as? String ?? previous ?? "")
        case "commandExecution":
            var fields: [String] = []
            if let command = item["command"] as? String { fields.append(command) }
            if let cwd = item["cwd"] as? String { fields.append("目录：\(cwd)") }
            if let status = item["status"] as? String { fields.append("状态：\(status)") }
            if let exitCode = item["exitCode"], !(exitCode is NSNull) { fields.append("退出码：\(exitCode)") }
            if let output = item["aggregatedOutput"] as? String, !output.isEmpty { fields.append("输出：\n\(output)") }
            if let previous, !previous.isEmpty, !fields.contains(where: { $0.hasPrefix("输出：") }),
               let marker = previous.range(of: "\n输出：\n") {
                fields.append("输出：\n" + previous[marker.upperBound...])
            }
            return ("命令执行", fields.joined(separator: "\n"))
        case "fileChange":
            return ("文件变更", json(item))
        case "reasoning":
            return ("Reasoning", "推理内容不在插件面板展示。")
        default:
            return (type, json(item))
        }
    }

    private static func nestedStatus(_ value: [String: Any]) -> String {
        guard let turn = value["turn"] as? [String: Any] else { return "" }
        let status = turn["status"] as? String ?? "unknown"
        let id = turn["id"] as? String ?? ""
        return id.isEmpty ? status : "\(id) · \(status)"
    }

    private static func replacing(_ entries: [CodexTimelineEntry], with entry: CodexTimelineEntry) -> [CodexTimelineEntry] {
        var result = entries
        let boundedEntry = CodexTimelineEntry(id: entry.id, title: bounded(entry.title), detail: bounded(entry.detail),
            fileChanges: entry.fileChanges, omittedFileCount: entry.omittedFileCount,
            changeReference: entry.changeReference, changeUnavailable: entry.changeUnavailable)
        if let index = result.firstIndex(where: { $0.id == entry.id }) { result[index] = boundedEntry }
        else { result.append(boundedEntry) }
        while result.count > maximumEntries || result.reduce(0, { $0 + retainedBytes($1) }) > maximumTimelineBytes {
            guard !result.isEmpty else { break }
            let oldestOther = result.firstIndex(where: { $0.id != entry.id })
            guard let index = oldestOther ?? (result.count == 1 ? result.startIndex : nil) else { break }
            result.remove(at: index)
        }
        return result
    }

    private static func retainedBytes(_ entry: CodexTimelineEntry) -> Int {
        entry.title.utf8.count + entry.detail.utf8.count + entry.fileChanges.reduce(0) { bytes, file in
            bytes + file.path.utf8.count + (file.movePath?.utf8.count ?? 0) +
                file.lines.reduce(0) { $0 + $1.text.utf8.count }
        }
    }

    private static func bounded(_ value: String) -> String {
        guard value.utf8.count > maximumEntryBytes else { return value }
        return String(value.prefix(8000)) + "\n…（内容已截断）"
    }

    private static func json(_ value: Any) -> String {
        guard JSONSerialization.isValidJSONObject(value),
              let data = try? JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]) else {
            return ""
        }
        return bounded(String(decoding: data, as: UTF8.self))
    }
}

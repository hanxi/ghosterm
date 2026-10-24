// FilesDragDrop — 文件拖放的数据面（栏间拖放 + Finder 拖入）。
//
// 拖的是「哪一栏的哪个条目」；落到哪一栏决定动作。把这段决策抽成纯函数，
// 才能离线断言（真实拖放手势没法在无头环境里验）。
import Foundation

/// 被拖的一个条目。
public struct DragItem: Equatable, Sendable {
    public enum Side: String, Sendable { case local, remote }

    public let side: Side
    public let path: String
    public let name: String
    public let isDirectory: Bool

    public init(side: Side, path: String, name: String, isDirectory: Bool) {
        self.side = side
        self.path = path
        self.name = name
        self.isDirectory = isDirectory
    }

    /// 编码成单个字符串（拖放载荷用）。分隔符取换行——路径里不会有换行。
    public func encoded() -> String {
        "\(side.rawValue)\n\(isDirectory ? "1" : "0")\n\(ImageEscape.encode(path))\n\(ImageEscape.encode(name))"
    }

    public static func decode(_ s: String) -> DragItem? {
        let parts = s.split(separator: "\n", maxSplits: 3,
                            omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 4, let side = Side(rawValue: parts[0]) else { return nil }
        guard let path = ImageEscape.decode(parts[2]),
              let name = ImageEscape.decode(parts[3]) else { return nil }
        return DragItem(side: side, path: path, name: name, isDirectory: parts[1] == "1")
    }
}

/// 载荷里的路径转义（避免与分隔符冲突）。
enum ImageEscape {
    static func encode(_ s: String) -> String {
        s.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\n", with: "\\n")
    }
    static func decode(_ s: String) -> String? {
        var out = ""
        var it = s.makeIterator()
        while let c = it.next() {
            if c == "\\" {
                guard let n = it.next() else { return nil }
                out.append(n == "n" ? "\n" : n)
            } else { out.append(c) }
        }
        return out
    }
}

/// 拖放的落点动作。
public enum DropAction: Equatable {
    /// 把某远端路径下载到目标目录。
    case download(remote: String, name: String, isDirectory: Bool)
    /// 把某本地路径上传到目标目录。
    case upload(local: String, name: String, isDirectory: Bool)
    /// 不做（同栏内、或落点不接受）。
    case none
}

/// 决策：谁拖到谁。
public enum DropRouter {
    /// 应用内部拖放：远端条目落到本地栏 = 下载；本地条目落到远端栏 = 上传；
    /// 同栏互拖 = 不做（栏内移动/重命名是另一件事，v1 不做）。
    public static func route(item: DragItem, onto side: DragItem.Side) -> DropAction {
        switch (item.side, side) {
        case (.remote, .local):
            return .download(remote: item.path, name: item.name,
                             isDirectory: item.isDirectory)
        case (.local, .remote):
            return .upload(local: item.path, name: item.name,
                           isDirectory: item.isDirectory)
        default:
            return .none
        }
    }

    /// 从 Finder 拖来的本地路径：落到远端栏 = 上传；落到本地栏 = 不做
    /// （本地栏本来就看得见它）。
    public static func route(finderPath: String, name: String, isDirectory: Bool,
                             onto side: DragItem.Side) -> DropAction {
        guard side == .remote else { return .none }
        return .upload(local: finderPath, name: name, isDirectory: isDirectory)
    }
}

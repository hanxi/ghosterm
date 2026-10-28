import Foundation

/// 新建入口的选择结果；窗口先关闭选择器，再执行，避免重叠的模态对话框。
public struct ConnectionRequest {
    public enum Kind: String, CaseIterable, Identifiable {
        case terminal, files, tmux
        public var id: Self { self }
        public var title: String {
            switch self {
            case .terminal: return "终端"
            case .files: return "文件"
            case .tmux: return "后台会话"
            }
        }
    }
    public let target: AppModel.Target
    public let kind: Kind
    public init(target: AppModel.Target, kind: Kind) { self.target = target; self.kind = kind }

    @MainActor public func open(in model: AppModel) throws {
        if case .host(let pattern) = target {
            if let error = model.loadError { throw ConnectionError.unavailable(error) }
            guard model.hosts.contains(where: { $0.pattern == pattern }) else {
                throw ConnectionError.unavailable("主机「\(pattern)」已不在当前 SSH 配置中，请重新选择。")
            }
        }
        switch kind {
        case .terminal: model.connect(to: target)
        case .tmux: model.showTmux(for: target)
        case .files:
            switch target {
            case .localShell: model.openLocalFiles(newTab: true)
            case .host(let pattern): model.openSFTP(for: pattern)
            }
        }
    }
}

private enum ConnectionError: LocalizedError {
    case unavailable(String)
    var errorDescription: String? { if case .unavailable(let message) = self { return message }; return nil }
}

public enum ConnectionSearch {
    /// 每个词都须匹配；大小写与重音不影响查找，不改变配置中的原始别名。
    public static func matches(_ query: String, text: String) -> Bool {
        query.split(whereSeparator: \.isWhitespace).allSatisfy {
            text.range(of: String($0), options: [.caseInsensitive, .diacriticInsensitive]) != nil
        }
    }
}

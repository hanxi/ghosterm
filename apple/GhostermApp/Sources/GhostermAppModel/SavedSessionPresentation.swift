import Foundation

public extension SessionRecord {
    var displayTitle: String {
        let title = target?.alias ?? "本地 shell"
        return session > 0 ? title + " · \(session)" : title
    }
    /// 已结束的会话在最后一个客户端脱离后会被 daemon 立即回收，恢复只会得到
    /// 「找不到」。这里如实标为不可恢复，不承诺一个点不动的入口。
    var canRestore: Bool { disposition != .ended && (session > 0 || admitted) }
    var stateLabel: String {
        switch disposition {
        case .session: return session > 0 ? "可恢复" : admitted ? "创建结果待查询" : "创建未确认"
        case .stopUnknown: return "停止结果未知"
        case .stopPending: return "正在停止"
        case .stopRejected: return "停止被拒绝"
        case .ended: return exitCode.map { "已退出（\($0)）" } ?? "已结束（退出码未知）"
        }
    }
}

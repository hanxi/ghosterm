// ProgramStatusStore —— OSC 7501（Program Status Protocol）的记录面。
//
// 职责边界（铁律 #1）：terminal 只把报告**搬过来**（TerminalSurface.onProgramStatus），
// 按 id 维护记录、决定「现在该显示什么」是 App 层的事。这一层是**纯模型**：
// 不碰 UI、不碰 libghostty，故可以离线把规范的每条规则正面测掉。
//
// 规范要求（Program Status Protocol, spec rev 0.4）：
//   1. 每条报告**整条替换**它 id 的记录——报告没发的字段就是没了，不保留旧值；
//   2. `clear` 不是状态：删掉该 id 的记录**与它下面所有记录**
//      （清 "build" 也清 "build/test"）；id 为空则清空全部；
//   3. 记录没带 app 时，app 取**最近的有 app 的祖先**；
//   4. 最多保留 256 条；超了丢「最久没更新」的那条；
//   5. 提示符开始 / 程序退出时，清掉 working/blocked 记录（done/error 留到用户看过）；
//   6. 全量复位（RIS）→ 等价于一条空 id 的 clear。
import Foundation
import GhostermTerminal

/// 按 id 维护的 OSC 7501 记录集。值类型：pane 上一份，`@Published` 直接吃。
public struct ProgramStatusStore: Equatable, Sendable {

    /// 一条记录。字段与 `ProgramStatusReport` 对齐；`app` 已经做过祖先继承。
    public struct Record: Equatable, Sendable, Identifiable {
        public var id: String
        public var app: String
        public var state: ProgramStatusReport.State
        public var kind: ProgramStatusReport.Kind
        /// 0…100，或 -1（不定 / 不适用）。
        public var progress: Int
        public var title: String
        public var message: String
        /// 单调递增的更新序号，仅用于「最久没更新」的淘汰。
        var updatedAt: UInt64

        /// 显式 public（Record 跨模块造样本 / 测试用）。
        public init(id: String, app: String, state: ProgramStatusReport.State,
                    kind: ProgramStatusReport.Kind, progress: Int,
                    title: String, message: String, updatedAt: UInt64 = 0) {
            self.id = id; self.app = app; self.state = state; self.kind = kind
            self.progress = progress; self.title = title; self.message = message
            self.updatedAt = updatedAt
        }
    }

    /// 规范给的上限（至少允许 64，最多 256）。
    public static let capacity = 256

    public private(set) var records: [Record] = []

    private var seq: UInt64 = 0

    public init() {}

    public var isEmpty: Bool { records.isEmpty }

    /// 应用一条报告。`clear` 走删除路径；其余整条替换。
    public mutating func apply(_ report: ProgramStatusReport) {
        if report.state == .clear {
            clear(id: report.id)
            return
        }

        seq += 1
        let record = Record(
            id: report.id,
            app: report.app.isEmpty ? inheritedApp(of: report.id) : report.app,
            state: report.state,
            kind: report.kind,
            progress: report.progress,
            title: report.title,
            message: report.message,
            updatedAt: seq)

        if let i = records.firstIndex(where: { $0.id == report.id }) {
            records[i] = record            // 整条替换：没发的字段不留旧值
        } else {
            records.append(record)
        }
        evictIfNeeded()
    }

    /// 删掉 `id` 的记录**与它下面所有记录**；`id` 为空 = 清空全部。
    public mutating func clear(id: String) {
        guard !id.isEmpty else { records.removeAll(); return }
        let prefix = id + "/"
        records.removeAll { $0.id == id || $0.id.hasPrefix(prefix) }
    }

    /// 清掉 working/blocked（提示符开始 / 程序退出时用）。done/error 保留。
    public mutating func removeEphemeral() {
        records.removeAll { $0.state == .working || $0.state == .blocked }
    }

    /// 记录没带 app 时，从「最近的有 app 的祖先」继承（`build/test` ← `build`）。
    private func inheritedApp(of id: String) -> String {
        var parts = id.split(separator: "/").map(String.init)
        while parts.count > 1 {
            parts.removeLast()
            let ancestor = parts.joined(separator: "/")
            if let r = records.first(where: { $0.id == ancestor }), !r.app.isEmpty {
                return r.app
            }
        }
        return ""
    }

    /// 超过上限就丢最久没更新的（`updatedAt` 最小者）。
    private mutating func evictIfNeeded() {
        while records.count > Self.capacity {
            guard let i = records.indices.min(by: { records[$0].updatedAt < records[$1].updatedAt })
            else { return }
            records.remove(at: i)
        }
    }

    /// 给 UI 的**一句话摘要**：挑「最需要用户注意」的一条。
    ///
    /// 优先级显式排定，不靠遍历顺序（同 `PaneStatus.worst` 的思路）：
    ///   blocked（等用户） > error > working > done > idle。
    /// 同级里取最近更新的。
    public var summary: Record? {
        func rank(_ s: ProgramStatusReport.State) -> Int {
            switch s {
            case .blocked: return 4
            case .error: return 3
            case .working: return 2
            case .done: return 1
            case .idle: return 0
            case .clear: return -1   // 不该出现在记录里
            }
        }
        return records.max { a, b in
            let (ra, rb) = (rank(a.state), rank(b.state))
            return ra == rb ? a.updatedAt < b.updatedAt : ra < rb
        }
    }
}

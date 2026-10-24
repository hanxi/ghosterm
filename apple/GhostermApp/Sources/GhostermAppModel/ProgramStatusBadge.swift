// ProgramStatusBadge —— 程序状态（OSC 7501）在 UI 上的**可测描述**。
//
// 为什么把「画什么」抽成纯模型：SwiftUI 视图本身在无头闸门里不好断言，
// 于是把**决策**（什么状态 → 什么图标 / 文案 / 是否带进度）放进纯函数，
// 正面测掉；视图只做「照它画」。这与 PaneStatus.worst 抽成纯函数同口径。
//
// 规范点名的一条：程序给的 title/message 是**不可信文本**，若显示在终端
// 之外（标签页 / 通知），要 (a) 去掉不可见格式字符，(b) 标明文本来自哪个
// 终端——免得某个程序假装自己是别处的程序。两者都在这里落实。
import Foundation

public struct ProgramStatusBadge: Equatable, Sendable {
    public enum Kind: Equatable, Sendable {
        case working      // 在干活（可能带百分比）
        case blocked      // 等用户（要人管）
        case done         // 有结果待看
        case error        // 失败停了
    }

    public var kind: Kind
    /// SF Symbol 名（视图照画，不再自己判断）。
    public var symbol: String
    /// 无障碍 / hover 提示：一句话，已清理并标明来源。
    public var label: String
    /// 仅 working/blocked 且程序给了百分比时非 nil。
    public var progress: Int?

    /// `record` 为 nil 或 idle 时不给角标（终端里什么都不显示）。
    public init?(record: ProgramStatusStore.Record?,
                 terminalName: String = "") {
        guard let r = record else { return nil }
        switch r.state {
        case .working: self.kind = .working
        case .blocked: self.kind = .blocked
        case .done: self.kind = .done
        case .error: self.kind = .error
        case .idle, .clear: return nil
        }

        switch kind {
        case .working:
            symbol = r.progress >= 0 ? "arrow.triangle.2.circlepath" : "clock"
        case .blocked:
            // 需要用户动手——用 FILL 的符号，视觉上更「催」。
            symbol = "exclamationmark.circle.fill"
        case .done:
            symbol = "checkmark.circle.fill"
        case .error:
            symbol = "xmark.octagon.fill"
        }
        progress = (kind == .working || kind == .blocked) && r.progress >= 0 ? r.progress : nil

        // 文案：程序名 / 标题 / 说明都可能为空，逐级回落。
        let what = Self.clean(r.title).isEmpty ? Self.clean(r.app) : Self.clean(r.title)
        let detail = Self.clean(r.message)
        var text = what
        if !detail.isEmpty { text = text.isEmpty ? detail : "\(what)：\(detail)" }
        if text.isEmpty {
            switch kind {
            case .working: text = "工作中"
            case .blocked: text = "等待你处理"
            case .done: text = "有结果"
            case .error: text = "出错了"
            }
        }
        if let p = progress { text += "（\(p)%）" }
        // 标明来源：这是规范要求的 (b)。
        label = terminalName.isEmpty ? text : "\(terminalName) · \(text)"
    }

    /// 去掉**不可见格式字符**（方向覆盖 / bidi 控制等）——规范点名要求。
    /// 只做显示层清理，不改记录本身。
    public static func clean(_ s: String) -> String {
        var out = String.UnicodeScalarView()
        for u in s.unicodeScalars where !isInvisibleFormat(u) {
            out.append(u)
        }
        return String(out).trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func isInvisibleFormat(_ u: Unicode.Scalar) -> Bool {
        switch u.value {
        case 0x061C,                                  // ARABIC LETTER MARK
             0x200B...0x200F,                         // ZWSP..RLM（含 LRM/RLM）
             0x2028, 0x2029,                          // 行/段分隔符
             0x202A...0x202E,                         // LRE/RLE/PDF/LRO/RLO
             0x2066...0x2069,                         // LRI/RLI/FSI/PDI
             0xFEFF:                                  // BOM / ZWNBSP
            return true
        default:
            return false
        }
    }
}

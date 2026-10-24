// FlowLayout — 「一行排不下就折到下一行」的横向布局。
//
// 为什么需要它（用户实测）：文件面板顶部的工具条是**一行硬排**（HStack），
// 而面板可以窄到 260pt（双栏里每栏更窄）——按钮加起来远超一行，超出的部分
// 被画到框外，看上去就是「顶部按钮超框了」。HStack 既不报错也不折行，这条
// 只在屏幕上看得出。
//
// 分行规则抽成纯函数 `FlowLayout.rows(...)`：视图照它排版，测试照它断言
// （`FilesPanelTests`），故「排不下必须折行」能被钉住，而不是靠肉眼。
import SwiftUI

struct FlowLayout: Layout {
    var spacing: CGFloat = DS.Padding.xs
    var lineSpacing: CGFloat = DS.Padding.xs

    /// **分行规则（纯函数）**：给定每项的宽度、可用宽度与项间距，返回每一行
    /// 放哪几项（下标，保持原顺序）。
    ///
    /// 语义：贪心按序填行；放不下就另起一行。**单项比整行还宽**时它独占一行
    /// （不裁、不缩——裁了就看不到那个按钮了，那正是要修的毛病）。
    static func rows(widths: [CGFloat], available: CGFloat,
                     spacing: CGFloat) -> [[Int]] {
        var rows: [[Int]] = []
        var current: [Int] = []
        var used: CGFloat = 0
        for (i, w) in widths.enumerated() {
            if current.isEmpty {
                current = [i]; used = w
            } else if used + spacing + w <= available {
                current.append(i); used += spacing + w
            } else {
                rows.append(current); current = [i]; used = w
            }
        }
        if !current.isEmpty { rows.append(current) }
        return rows
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout Void) -> CGSize {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        let available = proposal.width ?? .infinity
        let rows = Self.rows(widths: sizes.map(\.width), available: available,
                             spacing: spacing)
        var height: CGFloat = 0
        for (i, row) in rows.enumerated() {
            height += (row.map { sizes[$0].height }.max() ?? 0)
                + (i == 0 ? 0 : lineSpacing)
        }
        let contentWidth = rows.map { row in
            row.reduce(0) { $0 + sizes[$1].width }
                + CGFloat(max(0, row.count - 1)) * spacing
        }.max() ?? 0
        return CGSize(width: proposal.width ?? contentWidth, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout Void) {
        let sizes = subviews.map { $0.sizeThatFits(.unspecified) }
        let rows = Self.rows(widths: sizes.map(\.width), available: bounds.width,
                             spacing: spacing)
        var y = bounds.minY
        for row in rows {
            let rowHeight = row.map { sizes[$0].height }.max() ?? 0
            var x = bounds.minX
            for i in row {
                let size = sizes[i]
                subviews[i].place(
                    at: CGPoint(x: x, y: y + (rowHeight - size.height) / 2),
                    anchor: .topLeading,
                    proposal: ProposedViewSize(width: size.width, height: size.height))
                x += size.width + spacing
            }
            y += rowHeight + lineSpacing
        }
    }
}

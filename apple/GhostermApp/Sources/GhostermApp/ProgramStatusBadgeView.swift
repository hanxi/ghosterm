// ProgramStatusBadgeView —— 程序状态（OSC 7501）角标的画法。
//
// 决策（什么状态 → 什么图标/文案/进度、清理不可见字符、标明来源）全在
// ProgramStatusBadge（模型层，离线可测）。这里只做「照它画」。
import GhostermAppModel
import SwiftUI

struct ProgramStatusBadgeView: View {
    let badge: ProgramStatusBadge

    var body: some View {
        HStack(spacing: 2) {
            Image(systemName: badge.symbol)
                .font(DS.Font.captionSmall)
                .foregroundStyle(tint)
            if let p = badge.progress {
                Text("\(p)%")
                    .font(DS.Font.captionSmall)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
        }
        .help(badge.label)
        .accessibilityLabel(badge.label)
    }

    /// 颜色只是提示，语义由图标 + 文案承载（三态不折叠的同一条纪律）。
    private var tint: Color {
        switch badge.kind {
        case .working: return .accentColor
        case .blocked: return .orange      // 等用户：要看得见
        case .done: return .green
        case .error: return .red
        }
    }
}

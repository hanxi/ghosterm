// Appearance.swift — 外观 / 界面字号 / 终端字号的档位（设置页「外观」页的数据面）。
//
// 参照 iGhostVT 的 `Backend/Theme/AppearancePreference.swift` 与 `TextSize.swift`：
//   · **外观模式**（跟随系统 / 浅色 / 深色）——同时喂两处：AppKit 的
//     `NSApp.appearance`（我们的界面）与 libghostty 的色板事件
//     （`ghostty_app_set_color_scheme`，决定 `theme = light:X,dark:Y` 里
//     选哪一套配色）。少喂一处就会出现「界面是深色、终端还是浅色」。
//   · **界面字号**——macOS 的 SwiftUI **不认** `dynamicTypeSize`（实测：
//     `.dynamicTypeSize(.xxLarge)` 渲染出来仍是 16pt，与默认一模一样），
//     所以像 Catalyst 上的 iGhostVT 一样自己乘一个系数；这个系数经
//     `DS.Font` 角色（见 DesignSystem.swift）落到每一处文字。
//   · **终端字号**——那是 Ghostty 配置里的事（`font-size`），不是界面字号：
//     两者写在不同地方（UserDefaults vs ghostty.conf），必须说清。
import AppKit
import Foundation

// MARK: - 外观模式

public enum AppearancePreference: String, CaseIterable, Identifiable {
    case system
    case light
    case dark

    public static let key = "Ghosterm.appearance"

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .system: return "跟随系统"
        case .light: return "浅色"
        case .dark: return "深色"
        }
    }

    /// 当前偏好（读不到就当跟随系统）。
    public static var current: AppearancePreference {
        UserDefaults.standard.string(forKey: key)
            .flatMap(AppearancePreference.init(rawValue:)) ?? .system
    }

    /// AppKit 的外观对象（nil = 跟随系统）。
    public var nsAppearance: NSAppearance? {
        switch self {
        case .system: return nil
        case .light: return NSAppearance(named: .aqua)
        case .dark: return NSAppearance(named: .darkAqua)
        }
    }

    /// 界面当前实际是深色还是浅色（跟随系统时由 AppKit 决定）。
    /// 这是喂给 libghostty 的那一位——`theme = light:X,dark:Y` 靠它选边。
    @MainActor
    public static func isDarkNow(_ preference: AppearancePreference) -> Bool {
        switch preference {
        case .light: return false
        case .dark: return true
        case .system:
            // 用 `NSApplication.shared`（而不是 `NSApp`）：后者在 NSApplication
            // 还没被创建时是 nil（无窗口的测试进程里就是这样，实测这里崩过一次）。
            // `shared` 会按需创建它，语义与「跟随系统」不冲突。
            let best = NSApplication.shared.effectiveAppearance
                .bestMatch(from: [.aqua, .darkAqua])
            return best == .darkAqua
        }
    }

    /// 把偏好落到 AppKit 上（我们的界面）。返回实际是否深色，供调用方
    /// 同步给终端引擎。
    @MainActor
    @discardableResult
    public func applyToApplication() -> Bool {
        NSApp.appearance = nsAppearance
        return AppearancePreference.isDarkNow(self)
    }
}

// MARK: - 界面字号

/// 界面文字大小的档位。0 = 系统基准；每档 ±10%。
public enum InterfaceTextSize {
    public static let key = "Ghosterm.interfaceTextSize"
    public static let defaultStep = 0
    /// 档位范围（-2 = 80%，+4 = 140%）。
    public static let steps: ClosedRange<Int> = -2...4

    /// 第 `step` 档的乘数。
    public static func scale(step: Int) -> CGFloat {
        let clamped = min(max(step, steps.lowerBound), steps.upperBound)
        return 1 + 0.1 * CGFloat(clamped)
    }

    /// 档位的百分比写法（界面回显用）。
    public static func percent(step: Int) -> String {
        "\(Int((scale(step: step) * 100).rounded()))%"
    }
}

// MARK: - 终端字号

/// 终端字号（写进 ghostty.conf 的 `font-size`）。
public enum TerminalFontSize {
    /// 与 `SettingsStore.terminalSettingKeys` 里 `font-size` 的范围一致。
    public static let range: ClosedRange<Double> = 6...48
}
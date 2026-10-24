// ThemeSelection — 终端配色的读写（`theme` 键的解析与序列化）。
//
// 为什么要单独一层：libghostty 的 `theme` 有**两种写法**，而且是一段真语法，
// 不是"一个名字"：
//   theme = "Name"                      —— 浅色深色都用它
//   theme = light:"A",dark:"B"          —— 两档各一套
// 而且第二形态是**半结构化的**（引号、逗号、冒号都要对）。设置页给了两个
// 下拉（浅色 / 深色），拼错了整份配置就废（libghostty 会报诊断 → 设置保存
// 失败），所以拼装与解析都放这里，配断言（`ThemeSelectionTests`）。
//
// 语义细节以 vendor/ghostty/src/config/Config.zig 的 `Theme.parseCLI` 为准：
//   · 值里出现 `,` / `=` / `:` 才按 `light:X,dark:Y` 解析；
//   · 值被引号整体包住时剥掉引号；
//   · `light` 与 `dark` 都**没有默认值**，所以配对形态必须两边都给，
//     只给一边是 InvalidValue（不是"另一边润默认"）。
import Foundation

public struct ThemeSelection: Equatable {
    /// 浅色档选的配色名（nil = 没选）。
    public var light: String?
    /// 深色档选的配色名（nil = 没选）。
    public var dark: String?

    public init(light: String? = nil, dark: String? = nil) {
        self.light = light
        self.dark = dark
    }

    /// 两档都没选。
    public static let none = ThemeSelection()

    /// 解析配置里的 `theme` 值（含引号）。空串 → `.none`。
    public static func parse(_ raw: String?) -> ThemeSelection {
        guard let raw, !raw.isEmpty else { return .none }
        // 有分隔符 → 按 light/dark 对解析；否则是单值。
        if raw.contains(",") || raw.contains("=") || raw.contains(":") {
            var sel = ThemeSelection()
            for pair in raw.split(separator: ",") {
                let parts = pair.split(separator: ":", maxSplits: 1)
                guard parts.count == 2 else { continue }
                let key = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
                let value = stripQuotes(String(parts[1]).trimmingCharacters(in: .whitespaces))
                guard !value.isEmpty else { continue }
                switch key {
                case "light": sel.light = value
                case "dark": sel.dark = value
                default: break   // 不认识的键：忽略（不猜）
                }
            }
            return sel
        }
        let single = stripQuotes(raw.trimmingCharacters(in: .whitespaces))
        return single.isEmpty ? .none : ThemeSelection(light: single, dark: single)
    }

    /// 写回配置的值（**带引号**——配色名里有空格）。都没选 → nil（删除该行）。
    ///
    /// 只选了一档时**两档都用它**（而不是写一个缺一半的配对——那是非法值）。
    /// UI 的脚注里把这条说清楚，不让用户以为另一档没生效是 bug。
    public var configValue: String? {
        switch (light, dark) {
        case (nil, nil):
            return nil
        case (let l?, let d?) where l == d:
            return quote(l)
        case (let l?, let d?):
            return "light:\(quote(l)),dark:\(quote(d))"
        case (let only?, nil), (nil, let only?):
            return quote(only)
        }
    }

    /// 这一档**实际生效**的配色名。
    ///
    /// 两档都选了就各报各的；**只选了一档**时才落到另一档
    /// （只写一个名字时 libghostty 的语义就是两档同款，UI 得说实话）。
    ///
    /// 这里**必须分档**（踩过的坑）：早先写成 `light ?? dark`，参数根本没用上
    /// ——于是两档都报浅色那个名字。用户配 `light:"Zenwritten Light",
    /// dark:"Adwaita Dark"` 时，改深色那一档确实写回了配置、终端也换了色，
    /// 但两个下拉的标题都还念「Zenwritten Light」：看起来像没选上。
    public func effective(_ slot: ThemeSlot) -> String? {
        switch slot {
        case .light: return light ?? dark
        case .dark: return dark ?? light
        }
    }

    private static func stripQuotes(_ s: String) -> String {
        guard s.count >= 2, s.hasPrefix("\""), s.hasSuffix("\"") else { return s }
        return String(s.dropFirst().dropLast())
    }

    private func quote(_ name: String) -> String { "\"\(name)\"" }
}

/// 配色档位（浅色 / 深色）。与外观偏好一起决定终端用哪一套。
public enum ThemeSlot: String, CaseIterable, Identifiable {
    case light
    case dark

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .light: return "浅色配色"
        case .dark: return "深色配色"
        }
    }
}
// PaneAppearance — 窗格间距、标题与焦点标记。
// 默认贴边；已有的间距偏好继续有效，留白不影响配色。
import Foundation

// MARK: - 窗格间距
public enum PaneGap {
    public static let key = "Ghosterm.paneGap"
    public static let defaultValue = 0
    public static let standard = 10
    public static let steps: [Int] = [0, 6, 10, 14]
    public static func title(of value: Int) -> String {
        switch value {
        case 0: return "无"
        case 6: return "紧凑"
        case 14: return "宽松"
        default: return "标准"
        }
    }
    public static func resolved(_ raw: Int) -> Int {
        steps.contains(raw) ? raw : defaultValue
    }
    public static func cornerRadius(gap: Int) -> CGFloat { gap == 0 ? 0 : 8 }
}

// MARK: - 窗格标题栏

/// 窗格标题栏（状态点 + 进程名 + 目录 + 右侧按钮）。
public enum PaneTitleBar {
    /// 画不画标题栏。默认**开**，理由是远程场景：本地四个 zsh 分不清
    /// 无所谓，但「哪一格是 prod-web-01」必须一眼看出来。
    public static let visibleKey = "Ghosterm.paneTitleBarVisible"
    public static let visibleDefault = true

    /// 标题栏高度。**不可配**：可调只会让布局断言炸成一片，
    /// 而它本来就只有「够放一行小字」这一个正确值。
    public static let height: CGFloat = 26


}

// MARK: - 分屏时「哪格是当前」

/// 只有分屏中的当前窗格强调顶边；有间距时辅以淡蓝轮廓。
public enum PaneFocusChrome {
    public static func showsFocus(isFocused: Bool, isSplit: Bool) -> Bool {
        isFocused && isSplit
    }
}

// MARK: - 窗格标题的取值

/// 窗格标题栏上那两段文字。
public struct PaneTitleText: Equatable {
    /// 主名字（加粗那段）。
    public let name: String
    /// 跟在后面的灰字（目录）；nil = 没有可说的，不占位。
    public let detail: String?

    public init(name: String, detail: String?) {
        self.name = name
        self.detail = detail
    }
}

/// 窗格标题怎么取：**进程 → 目录 → 标签页标题**。
///
/// 三级回落都是「说得出来的事实」，没有一级是猜的：
///   · 进程名来自 shell 集成（OSC 133）报的命令行，且**只在命令还在跑时**
///     才算（命令结束后那行字还在 `lastCommand` 里，但它已经不是「这格在
///     跑什么」了）；
///   · 目录来自本地 PTY 的内核查询或远端的 OSC 7；
///   · 两样都没有（没装 shell 集成的 SSH、刚连上还没报）就退回标签页标题。
public enum PaneTitleRule {
    public static func resolve(program: String?, directory: String?,
                               tabTitle: String) -> PaneTitleText {
        let dir = abbreviate(directory)
        if let program, !program.isEmpty {
            return PaneTitleText(name: program, detail: dir)
        }
        if let dir {
            // 退到目录这一级时**不再**把同一个目录画两遍。
            return PaneTitleText(name: dir, detail: nil)
        }
        return PaneTitleText(name: tabTitle, detail: nil)
    }

    /// 从 shell 报的命令行里取程序名。
    ///
    /// 只做两件保守的事：跳过前置的 `FOO=bar` 环境赋值、取路径的最后一段
    /// （`/usr/local/bin/npm` → `npm`）。**不**解析参数、不认 `sudo`——
    /// 那是在猜用户的意图，猜错了标题就在撒谎。
    public static func programName(commandLine: String) -> String? {
        let cleaned = ProgramStatusBadge.clean(commandLine)
        for token in cleaned.split(separator: " ") {
            if token.contains("=") { continue }     // 前置环境赋值
            let name = (String(token) as NSString).lastPathComponent
            guard !name.isEmpty else { continue }
            return String(name.prefix(32))
        }
        return nil
    }

    /// 目录的显示形态：家目录缩成 `~`。空 / nil → nil（不占位）。
    public static func abbreviate(_ directory: String?,
                                  home: String = NSHomeDirectory()) -> String? {
        guard let directory, !directory.isEmpty else { return nil }
        guard !home.isEmpty else { return directory }
        if directory == home { return "~" }
        if directory.hasPrefix(home + "/") {
            return "~" + directory.dropFirst(home.count)
        }
        return directory
    }
}

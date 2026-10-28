// PaneAppearance — 窗格外观的两项偏好 + 窗格标题的取值规则。
//
// 为什么这几项是**偏好**而不是写死：改的是「每天盯 8 小时的那块地方」，
// 审美分歧必然存在。它们各自都能调到**最保守的一端**，调满之后屏幕上
// 就完全回到加这套卡片之前的样子（间距 0 + 不画标题栏）——
// 于是「我不喜欢」有出路，不必靠赌一个默认值。
//
// 规则一律抽成**纯函数**：视图与测试走同一条路径，不各写一遍
// （同 SplitDim / TerminalTab.worst 的口径）。
//
// 读写都用 `@AppStorage(key)` + 字面默认值：键不存在时 SwiftUI 用那个
// 默认值。**不能**拿 `UserDefaults.integer(forKey:)` 当间距——它读不到时
// 给 0，而 0 在这里是个合法档位（「无间距」），于是「没设置过」会被
// 当成「用户选了无间距」。
import Foundation

// MARK: - 窗格间距

/// 格与格、格与窗口边之间的留白。四个离散档位，不给连续滑块：
/// 连续值既没人能说出 7 和 8 的区别，也没法在设计稿里复核。
public enum PaneGap {
    public static let key = "Ghosterm.paneGap"

    /// 默认档（「标准」）。
    public static let standard = 10

    /// 全部档位（点）。
    public static let steps: [Int] = [0, 6, 10, 14]

    public static func title(of value: Int) -> String {
        switch value {
        case 0: return "无"
        case 6: return "紧凑"
        case 14: return "宽松"
        default: return "标准"
        }
    }

    /// 把存下来的值收回合法档位（手改 plist / 旧版本遗留值都可能越界）。
    public static func resolved(_ raw: Int) -> Int {
        steps.contains(raw) ? raw : standard
    }

    /// 间距为 0 时**退化形态**：不留圆角、不画卡片边框，只剩一条发丝
    /// 分隔线（见 PaneChrome）。圆角跟着间距走，不单独给开关——
    /// 贴边的卡片画圆角只会露出四个背景色的小缺口。
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

    /// 这一格要不要画右侧按钮：**只画聚焦那格**。
    ///
    /// 这里曾是一个「每一格都显示」的偏好，弃用的理由与顶栏偏好同源：
    /// 打开它得到的是四分屏 16 个图标 vs 1 个图标的差别——同一组动作在
    /// 四格里各画一遍，没有第二份信息。键**不读不写**，旧 plist 里存过的
    /// 值成为无害的死数据。
    public static func showsButtons(isFocused: Bool) -> Bool { isFocused }
}

// MARK: - 分屏时「哪格是当前」

/// 分屏时把当前窗格画出来的口径。
///
/// 为什么要专门有这一档：终端面自己的区分来自 ghostty 的
/// `unfocused-split-opacity`（见 `SplitDim`）——往非聚焦那格上盖一层**配置
/// 色**。可默认配置里盖的就是终端背景色本身，同色盖同色只剩文字淡三成；
/// 深色主题下这点淡化在一屏黑里基本读不出来。用户实测反馈「深色背景下非
/// 当前窗格不像未激活」。
///
/// 遮罩本身不动——那是上游语义，用户在 ghostty 配置里设什么就该是什么。
/// 区分改由 chrome 挑大梁：**聚焦那格**的标题栏点上强调色，非聚焦那格保持
/// 窗口底色。chrome 不吃终端配置，深浅外观下都成立。
///
/// 为什么是「点亮聚焦那格」而不是「压暗非聚焦那格」：压暗在深色外观下根本
/// 读不出来——标题栏底色本就接近黑（实测亮度 40），压 18% 只掉到 33，而同样
/// 一笔在浅色下要掉 46。区分必须两种外观下同样成立，所以信号放在**有色**
/// 这一侧：带色 vs 不带色与底色深浅无关。
public enum PaneFocusChrome {
    /// 聚焦那格标题栏叠的强调色不透明度。
    ///
    /// 只在分屏时有意义：只有一格时「哪格是当前」不是个问题，点色只是噪音
    /// （同 `PaneCard` 不画单格聚焦描边的口径）。
    public static func tintOpacity(isFocused: Bool, isSplit: Bool) -> Double {
        isFocused && isSplit ? 0.22 : 0
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

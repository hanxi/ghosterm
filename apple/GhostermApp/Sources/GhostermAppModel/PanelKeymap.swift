// PanelKeymap — 文件面板的键盘动作映射（纯函数，可离线断言）。
//
// 面板与终端可能同屏（内嵌侧栏），故这些键**只在面板的列表拿到焦点时**生效
// （视图侧用 onKeyPress，macOS 14+，挂在列表上），不挂窗口级——否则 ⌫/空格
// 会被从终端里偷走。macOS 13 上退化为右键菜单（诚实降级）。
import Foundation

public enum PanelAction: Equatable {
    case newFolder, rename, delete, info, search
}

public enum PanelKeymap {
    /// key 用小写字母；command/shift 是修饰键。命中返回动作，否则 nil。
    public static func action(key: String, command: Bool, shift: Bool) -> PanelAction? {
        switch (key.lowercased(), command, shift) {
        case ("n", true, true): return .newFolder      // ⇧⌘N
        case ("r", true, false): return .rename        // ⌘R
        case ("f", true, false): return .search        // ⌘F
        case ("i", true, false): return .info          // ⌘I
        case ("\u{8}", false, false): return .delete   // ⌫（无修饰）
        case ("\u{7f}", false, false): return .delete  // Delete（无修饰）
        default: return nil
        }
    }
}

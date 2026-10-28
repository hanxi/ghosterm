// ShortcutCatalog — 设置页「键盘」页列出的快捷键。
//
// 它列的是**真相**：每一条都必须能在主菜单（`AppMenu`）里找到同一个按键，
// 这条对应关系由 `SettingsUITests.testShortcutCatalogMatchesMenu` 双向钉住——
// 菜单里改了键而这里没跟着改，断言会红。所以这份清单不会变成一份会过期的
// 说明书。
//
// 只列**本应用自己的**动作（标签页 / 窗格 / 窗口 / 侧边栏）。菜单里的
// 「编辑」「窗口」等系统标准项（⌘C / ⌘V / ⌘Z…）不列——那是每个 macOS
// 应用都有的，写进来只是噪音。
import AppKit
import Foundation

public struct ShortcutItem {
    /// 做什么（与菜单项标题一致——断言按标题对）。
    public let title: String
    /// 按键写法（`⌘` 这种，与系统菜单的写法一致）。
    public let keys: String
}

public struct ShortcutGroup {
    public let title: String
    public let items: [ShortcutItem]
}

public enum ShortcutCatalog {
    public static let groups: [ShortcutGroup] = [
        ShortcutGroup(title: "标签页", items: [
            ShortcutItem(title: "新建标签页", keys: "⌘T"),
            ShortcutItem(title: "关闭标签页", keys: "⌘W"),
            ShortcutItem(title: "搜索标签页…", keys: "⇧⌘A"),
            ShortcutItem(title: "标签页 1 … 标签页 9", keys: "⌘1 … ⌘9"),
        ]),
        ShortcutGroup(title: "窗格", items: [
            ShortcutItem(title: "向右分屏", keys: "⌘D"),
            ShortcutItem(title: "向下分屏", keys: "⇧⌘D"),
            ShortcutItem(title: "缩放当前分屏", keys: "⇧⌘↩"),
            ShortcutItem(title: "关闭当前分屏", keys: "无"),
        ]),
        ShortcutGroup(title: "窗口与界面", items: [
            ShortcutItem(title: "关闭窗口", keys: "⇧⌘W"),
            ShortcutItem(title: "切换标签位置", keys: "⌥⌘S"),
            ShortcutItem(title: "文件面板", keys: "⇧⌘E"),
            ShortcutItem(title: "设置…", keys: "⌘,"),
            ShortcutItem(title: "退出 Ghosterm", keys: "⌘Q"),
        ]),
    ]

    /// 按键的规范写法（macOS 的惯例顺序：⌃⌥⇧⌘）。
    ///
    /// `keyEquivalent` 为空 = **没有**快捷键。注意不能只看修饰键：
    /// `NSMenuItem` 的 `keyEquivalentModifierMask` 默认就是 `.command`，
    /// 一个没有按键的菜单项照样带着它（实测踩过：这么写会把它显示成「⌘」）。
    public static func describe(keyEquivalent: String,
                                modifiers: NSEvent.ModifierFlags) -> String {
        guard !keyEquivalent.isEmpty else { return "无" }
        var s = ""
        if modifiers.contains(.control) { s += "⌃" }
        if modifiers.contains(.option) { s += "⌥" }
        if modifiers.contains(.shift) { s += "⇧" }
        if modifiers.contains(.command) { s += "⌘" }
        // 不可打印的键要用系统菜单上的那个字形，否则会原样画出控制字符
        // （回车会变成一个看不见的字符，菜单右侧像是「⇧⌘」后面没东西）。
        return s + (Self.glyphs[keyEquivalent] ?? keyEquivalent.uppercased())
    }

    private static let glyphs: [String: String] = [
        "\r": "↩",          // Return
        "\t": "⇥",          // Tab
        "\u{8}": "⌫",       // Delete
        "\u{1b}": "⎋",      // Esc
    ]
}

/// 关于页用的信息（版本 / 许可）。
public enum AboutInfo {
    /// "0.1.0-dev (123)" —— 从 Info.plist 读；开发态（直接跑可执行文件、
    /// 没有 bundle）**如实说不知道**，不编一个版本号。
    public static var versionDescription: String {
        let info = Bundle.main.infoDictionary
        guard let version = info?["CFBundleShortVersionString"] as? String,
              let build = info?["CFBundleVersion"] as? String else {
            return "开发构建（无 bundle 信息）"
        }
        return "\(version) (\(build))"
    }

    public struct Entry {
        public let name: String
        public let detail: String
    }

    /// 随包组件与各自的许可。按仓库的许可口径（AGENTS.md 铁律 #11 与
    /// docs/explanation/licensing.md）如实列出。
    public static let entries: [Entry] = [
        Entry(name: "Ghosterm", detail: "MIT License · 本程序"),
        Entry(name: "libghostty (Ghostty)", detail: "MIT · 终端引擎（含本地补丁）"),
        Entry(name: "libssh2", detail: "BSD-3-Clause · SSH / SFTP"),
        Entry(name: "OpenSSL", detail: "Apache-2.0 · SSH 加密后端"),
    ]
}
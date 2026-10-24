// SettingsUITests — 设置弹窗的数据面与「真能画出来」。
//
// 覆盖两类，分开写清：
//   1. **纯函数**：配色两档的解析 / 序列化（`theme` 是段真语法，拼错整份
//      配置就废）、`/etc/shells` 的解析、界面字号档位、页签清单。这些不碰
//      渲染，断言快而准。
//   2. **渲染**：把每一页真的装进 NSHostingView 并强制布局——视图里
//      `Menu` 的 label、`@ViewBuilder` 的分支、`@State` 的初始化都在这一步
//      才会执行，写错了（比如给 Menu 塞了不合法的 label）会在这里崩。
//
// 为什么值得写：设置页最容易「编译过、点开崩」，而它又是用户第一个会去翻的
// 地方。变异验证见 `scripts/mutation-check-settings-ui.sh`。
import AppKit
import Combine
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class SettingsUITests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        TestWindows.enterAutomationMode()
    }

    private func tempDir() throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-settings-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        return dir
    }

    @MainActor
    private func makeModel() throws -> AppModel {
        let dir = try tempDir()
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                            encoding: .utf8)
        return isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts",
                        settings: SettingsStore(terminalConfigPath: dir + "/ghostty.conf"))
    }

    // MARK: - 页签

    /// 页签就是这四项（会话 / 高级 / 关于**不再有**——删掉的页在这里钉住，
    /// 免得哪天又长回来）。
    ///
    /// 「关于」移出设置是有意的：它不是可调的设置项，macOS 的惯例是菜单项
    /// 直接开一张小窗口（见 `AboutWindow`）。
    func testPanesAreTheFourExpectedOnes() {
        XCTAssertEqual(SettingsView.Pane.allCases.map(\.rawValue),
                       ["通用", "外观", "键盘", "配置文件"],
                       "设置页签应为这四项（会话 / 高级 / 关于已移除）")
        // 每一项都得有自己的图标（标题栏那一排就是图标 + 名字）。
        for pane in SettingsView.Pane.allCases {
            XCTAssertFalse(pane.symbol.isEmpty, "\(pane.rawValue) 缺图标")
        }
        // **每一页都必须给高度**：不给的话窗口高度就由这一页的文字多少
        // 决定，切页时页签条会上下跳（用户实测报的「图标位置一直变」）。
        for p in SettingsView.Pane.allCases {
            XCTAssertGreaterThan(p.height, 100, "\(p.rawValue) 页没有高度——窗口会跟着内容跳")
        }
    }

    // MARK: - 配色（theme 的解析与序列化）

    /// 单值：两档都跟着它（libghostty 的语义：只写一个名字就是两档同款）。
    func testThemeSingleValueAppliesToBothSlots() {
        let sel = ThemeSelection.parse("\"Rose Pine\"")
        XCTAssertEqual(sel.light, "Rose Pine")
        XCTAssertEqual(sel.dark, "Rose Pine")
        XCTAssertEqual(sel.configValue, "\"Rose Pine\"",
                       "两档同款时应写回单值形态（引号不能少）")
        XCTAssertNil(ThemeSelection.parse("").configValue, "空 → 不设置（删行）")
        XCTAssertNil(ThemeSelection.parse(nil).configValue)
    }

    /// 配对：`light:X,dark:Y` 往返必须逐字一致——**这是拼格式，写错整份
    /// 配置就废**（libghostty 报诊断 → 保存直接失败）。
    func testThemePairRoundTrip() throws {
        let raw = "light:\"3024 Day\",dark:\"3024 Night\""
        let sel = ThemeSelection.parse(raw)
        XCTAssertEqual(sel.light, "3024 Day")
        XCTAssertEqual(sel.dark, "3024 Night")
        XCTAssertEqual(sel.configValue, raw, "配对形态应逐字往返")
    }

    /// 只选一档：**两档都用它**——不能写一个缺一半的配对
    /// （`light:X` 单独出现是 InvalidValue，会把设置页的保存打死）。
    func testThemeSingleSlotSelectionFallsBackToBoth() {
        var sel = ThemeSelection(light: "Solarized Light")
        XCTAssertEqual(sel.configValue, "\"Solarized Light\"")
        XCTAssertEqual(sel.effective(.dark), "Solarized Light",
                       "只选了浅色时深色档应显示同一套（UI 得说实话）")
        sel = ThemeSelection(dark: "Solarized Dark")
        XCTAssertEqual(sel.configValue, "\"Solarized Dark\"")
        XCTAssertEqual(sel.effective(.light), "Solarized Dark")
    }

    /// 两档**各选各的**时，每一档必须报**自己**那个名字。
    ///
    /// 这条护的是用户实测的「选了能生效，但下拉列表还显示原来那个」：
    /// `effective(_:)` 早先写成 `light ?? dark`——参数压根没用上，两档都
    /// 报浅色那个。于是配了 `light:"A",dark:"B"` 之后，改深色那档确实写回
    /// 了文件、终端也换了色，可两个下拉的标题都还念着 `A`，看着像没选上。
    func testThemeEffectiveReportsEachSlotIndependently() {
        let sel = ThemeSelection(light: "3024 Day", dark: "3024 Night")
        XCTAssertEqual(sel.effective(.light), "3024 Day",
                       "浅色档必须报浅色选的那个")
        XCTAssertEqual(sel.effective(.dark), "3024 Night",
                       "深色档必须报深色选的那个（不能落回浅色那个）")
        XCTAssertNotEqual(sel.effective(.light), sel.effective(.dark),
                          "两档选了不同配色时，两个下拉的标题必须不同")

        // 只选一档时的回落语义仍要成立（与上一条配套，防「修一半」）。
        let onlyDark = ThemeSelection(dark: "Adwaita Dark")
        XCTAssertEqual(onlyDark.effective(.light), "Adwaita Dark")
        XCTAssertEqual(onlyDark.effective(.dark), "Adwaita Dark")
    }

    /// 不认识的键忽略而不是猜（写错键名不该被当成某一档）。
    func testThemeUnknownSlotIgnored() {
        let sel = ThemeSelection.parse("light:\"A\",midnight:\"B\"")
        XCTAssertEqual(sel.light, "A")
        XCTAssertNil(sel.dark, "`midnight` 不是合法档位，不该被当成 dark")
    }

    // MARK: - 命令行解释器（/etc/shells）

    /// 注释与空行不算；重复路径只列一次；结果是稳定排序的绝对路径表。
    func testShellCatalogParsesEtcShells() {
        let text = """
        # List of acceptable shells

        /bin/bash
        /bin/zsh
        /bin/bash
        relative/not/absolute
        # /bin/comment
        /opt/homebrew/bin/bash
        """
        XCTAssertEqual(ShellCatalog.parse(text: text),
                       ["/bin/bash", "/bin/zsh", "/opt/homebrew/bin/bash"],
                       "只认绝对路径、去掉重复与注释，并排序")
        XCTAssertTrue(ShellCatalog.parse(text: "# 只有注释\n").isEmpty)
    }

    /// 下拉按钮上显示什么：空 = 自动；列表里没有的路径 = 自定义。
    func testShellCatalogCustomPathVisibility() {
        XCTAssertEqual(ShellCatalog.title(of: ""), "自动")
        XCTAssertEqual(ShellCatalog.title(of: "/bin/zsh"), "/bin/zsh")
        XCTAssertEqual(ShellCatalog.title(of: "/x", isCustom: true), "自定义")

        // 列表还没到（nil）时**不**显示自定义——不能因为「没读到」就误报。
        XCTAssertFalse(ShellCatalog.showsCustomPath("/opt/weird", available: nil,
                                                    isEditing: false))
        XCTAssertFalse(ShellCatalog.showsCustomPath("/bin/zsh",
                                                    available: ["/bin/zsh"],
                                                    isEditing: false))
        XCTAssertTrue(ShellCatalog.showsCustomPath("/opt/weird",
                                                   available: ["/bin/zsh"],
                                                   isEditing: false))
        XCTAssertTrue(ShellCatalog.showsCustomPath("", available: [],
                                                   isEditing: true),
                      "正在编辑自定义时就该显示输入框（哪怕值是空的）")
    }

    /// 切换「默认命令行解释器」后，设置页的下拉必须跟着刷新。
    ///
    /// 用户实测「命令行解释器切换后，下拉列表没有更新」：`AppModel.preferences`
    /// 早先是普通存储属性（非 @Published），且 `AppPreferences` 的 setter 是
    /// `nonmutating`（只写 UserDefaults、不重赋 struct）——`defaultShell` 改
    /// 了，`objectWillChange` 不发，SwiftUI 不重画，下拉标题与勾选都停在旧值。
    /// 修法：`preferences` 改 @Published，setter 去掉 nonmutating（变异的）→
    /// `model.preferences.xxx = y` 重赋这个 @Published 属性、触发广播。这条断
    /// 的是「改一个偏好后 `objectWillChange` 真的发了」。
    @MainActor
    func testPreferenceChangeRepublishes() throws {
        let model = try makeModel()
        // 用独立 defaults suite，别污染用户真实偏好。
        let suite = "dev.ghosterm.tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        model.preferences = AppPreferences(defaults: defaults)
        // 先订阅：上面那次整份赋值在订阅之前，不算。
        var fired = false
        let cancellable = model.objectWillChange.sink { _ in fired = true }
        defer { cancellable.cancel() }
        model.preferences.defaultShell = "/bin/zsh"
        XCTAssertTrue(fired,
                      "改默认 shell 后必须 objectWillChange（否则下拉不刷新）")
        XCTAssertEqual(model.preferences.defaultShell, "/bin/zsh",
                       "值应已写进 UserDefaults")
    }

    // MARK: - 字号

    /// 界面字号档位：0 = 100%，每档 ±10%，且**夹在范围内**。
    func testInterfaceTextScaleSteps() {
        XCTAssertEqual(InterfaceTextSize.scale(step: 0), 1.0)
        XCTAssertEqual(InterfaceTextSize.percent(step: 0), "100%")
        XCTAssertEqual(InterfaceTextSize.percent(step: 2), "120%")
        XCTAssertEqual(InterfaceTextSize.percent(step: -2), "80%")
        XCTAssertEqual(InterfaceTextSize.scale(step: 99),
                       InterfaceTextSize.scale(step: InterfaceTextSize.steps.upperBound),
                       "超出范围应夹到上界（不是无限放大）")
    }

    /// 终端的配色与字号跟界面字号**不是一回事**：`font-size` 仍是 Ghostty
    /// 配置里的键，范围与 SettingsStore 的那张表一致。
    func testTerminalFontSizeRangeMatchesSettingsTable() {
        let spec = SettingsStore.terminalSettingKeys.first { $0.key == "font-size" }
        guard case .number(let range, _)? = spec?.kind else {
            return XCTFail("terminalSettingKeys 里应有 font-size 的数值项")
        }
        XCTAssertEqual(range, TerminalFontSize.range,
                       "外观页的终端字号范围必须与 SettingsStore 的表一致")
    }

    // MARK: - 外观偏好

    func testAppearancePreferenceTitlesAndDefaults() {
        XCTAssertEqual(AppearancePreference.allCases.map(\.title),
                       ["跟随系统", "浅色", "深色"])
        XCTAssertNil(AppearancePreference.system.nsAppearance, "跟随系统 = 不设外观")
        XCTAssertEqual(AppearancePreference.dark.nsAppearance?.name,
                       NSAppearance.Name.darkAqua)
        XCTAssertEqual(AppearancePreference.light.nsAppearance?.name,
                       NSAppearance.Name.aqua)
    }

    /// 改外观偏好**必须立刻落到 AppKit**（不只是写 UserDefaults / 喂终端）。
    ///
    /// 用户实测「切深/浅色界面不动，重启才变」：`AppModel.appearance.didSet`
    /// 早先只喂了 libghostty（终端配色），没改 `NSApp.appearance`——于是界面
    /// 那半截要等下次启动 `applicationDidFinishLaunching` 的
    /// `applyToApplication()` 才生效。断言直接看 `NSApp.appearance` 有没有跟着
    /// 偏好走。
    @MainActor
    func testAppearancePreferenceAppliesToApplicationLive() throws {
        // 起手先把偏好钉成「跟随系统」：模型的初值读 UserDefaults，而
        // `didSet` 对「没变」的赋值会早退（那是对的——但会让本断言假绿/假红）。
        let saved = UserDefaults.standard.string(forKey: AppearancePreference.key)
        UserDefaults.standard.set(AppearancePreference.system.rawValue,
                                  forKey: AppearancePreference.key)
        defer {
            NSApp.appearance = nil
            if let saved { UserDefaults.standard.set(saved, forKey: AppearancePreference.key) }
            else { UserDefaults.standard.removeObject(forKey: AppearancePreference.key) }
        }
        let model = try makeModel()
        NSApp.appearance = NSAppearance(named: .aqua)
        model.appearance = .dark
        XCTAssertEqual(NSApp.appearance?.name, NSAppearance.Name.darkAqua,
                       "切到深色后 NSApp.appearance 必须立刻变深——否则界面要重启才变")
        model.appearance = .light
        XCTAssertEqual(NSApp.appearance?.name, NSAppearance.Name.aqua,
                       "切回浅色同理")
    }

    /// 设置窗口的底色**跟着外观走**（深色是深底、浅色是浅底）。
    ///
    /// 用户实测「重启后设置界面看不清了」：容器底色早先写成
    /// `NSColor.windowBackgroundColor.cgColor`——动态 `NSColor` 的 `.cgColor`
    /// 按 **`NSAppearance.current`**（当前绘制上下文的外观，通常=系统外观）解，
    /// 不是按窗口的 `effectiveAppearance`。系统浅色 + 用户选深色时它解出**白色**
    /// 底，而 SwiftUI 的文字按深色渲染成浅色 → 白底浅字，什么都看不清。
    /// 修法是把底色挪到绘制期解（`SettingsBackgroundView.updateLayer`）并在
    /// 外观变化时重画。这里直接量窗口底部的像素做判据（对照：深浅两档必须
    /// 一亮一暗）。
    @MainActor
    func testSettingsBackgroundTracksAppearance() throws {
        let model = try makeModel()
        defer { SettingsWindow.close(); NSApp.appearance = nil }
        SettingsWindow.close()
        NSApp.appearance = NSAppearance(named: .darkAqua)
        SettingsWindow.open(model: model, pane: .general)
        for _ in 0..<30 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        guard let content = SettingsWindow.controller?.window?.contentView else {
            return XCTFail("没拿到设置窗口")
        }
        func bottomLuma() -> Double? {
            guard let rep = content.bitmapImageRepForCachingDisplay(in: content.bounds)
            else { return nil }
            content.cacheDisplay(in: content.bounds, to: rep)   // 先抓帧，再取位图
            // 取底部一行的中间点：那里是容器自己的底（内容为空的留白区）。
            guard let c = rep.colorAt(x: Int(content.bounds.width * 0.5),
                                      y: Int(content.bounds.height * 0.97))?
                .usingColorSpace(.deviceRGB) else { return nil }
            return c.redComponent * 0.3 + c.greenComponent * 0.59 + c.blueComponent * 0.11
        }
        guard let dark = bottomLuma() else { throw XCTSkip("拿不到位图缓存（环境限制）") }
        NSApp.appearance = NSAppearance(named: .aqua)
        for _ in 0..<30 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        guard let light = bottomLuma() else { throw XCTSkip("拿不到位图缓存（环境限制）") }
        XCTAssertLessThan(dark, 0.5,
                          "深色外观下设置窗口底必须是深色（现在是 \(dark)——白底浅字就是用户报的「看不清」")
        XCTAssertGreaterThan(light, 0.5, "浅色外观下必须是浅色底")
    }

    // MARK: - 键盘 / 关于

    /// 键盘页列出的每一条都必须给了「在哪实现」——这份清单是真相清单，
    /// 不是说明书（没实现的组合不许列进来）。
    func testShortcutCatalogEntriesAreComplete() {
        XCTAssertFalse(ShortcutCatalog.groups.isEmpty)
        for group in ShortcutCatalog.groups {
            XCTAssertFalse(group.items.isEmpty, "\(group.title) 组是空的")
            for item in group.items {
                XCTAssertFalse(item.title.isEmpty)
                XCTAssertFalse(item.keys.isEmpty)
            }
        }
    }

    /// 按键写法按 macOS 惯例的顺序（⌃⌥⇧⌘ + 大写字母）。
    func testShortcutDescribeFormatting() {
        XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: "d",
                                                modifiers: [.command]), "⌘D")
        XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: "d",
                                                modifiers: [.command, .shift]),
                       "⇧⌘D")
        XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: "s",
                                                modifiers: [.command, .option]),
                       "⌥⌘S")
        XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: "",
                                                modifiers: []), "无")
    }

    /// 键盘页与「关于」页列出的每一条都必须是**真的**。
    ///
    /// 键盘页那张表最容易变成会过期的说明书：菜单里改了键，说明书不会跟着
    /// 变，用户照着按没反应。所以这里拿它跟**真装上的主菜单**双向对拍：
    /// 表里每条都要在菜单里找到同名项，且按键写法一致。
    @MainActor
    func testShortcutCatalogMatchesTheRealMenu() throws {
        let model = try makeModel()
        AppMenu.install(model: model)
        defer { NSApp.mainMenu = nil }

        // 把菜单树摊平成「标题 → 按键写法」。
        var byTitle: [String: String] = [:]
        func walk(_ menu: NSMenu) {
            for item in menu.items {
                if !item.title.isEmpty, item.action != nil {
                    byTitle[item.title] = ShortcutCatalog.describe(
                        keyEquivalent: item.keyEquivalent,
                        modifiers: item.keyEquivalentModifierMask)
                } else if item.title.hasPrefix("标签页 ") {
                    // ⌘1…⌘9 在菜单里是九条；表里写的是一条。
                    byTitle["标签页 \(item.keyEquivalent)"] = ShortcutCatalog.describe(
                        keyEquivalent: item.keyEquivalent,
                        modifiers: item.keyEquivalentModifierMask)
                }
                if let sub = item.submenu { walk(sub) }
            }
        }
        if let main = NSApp.mainMenu { walk(main) }

        for group in ShortcutCatalog.groups {
            for item in group.items {
                if item.title.contains("…") {
                    // 「标签页 1 … 标签页 9」是概述：逐条抽验两端。
                    for n in ["1", "9"] {
                        XCTAssertEqual(byTitle["标签页 \(n)"], "⌘\(n)",
                                       "键盘表说 ⌘\(n) 能切标签页，菜单里却不是")
                    }
                    continue
                }
                guard let actual = byTitle[item.title] else {
                    XCTFail("键盘表列了「\(item.title)」，但主菜单里没有这一项"
                            + "——说明表过期了（或者菜单漏了它）")
                    continue
                }
                XCTAssertEqual(actual, item.keys,
                               "「\(item.title)」的按键：表里写 \(item.keys)，"
                               + "菜单里是 \(actual)")
            }
        }
    }

    // MARK: - 渲染（每一页都真的装进视图树）

    /// 设置窗口的**几何**：内容必须贴着窗口顶（否则页签条会随窗口高度浮动）。
    ///
    /// 这条护的是用户实测报的「标签页图标位置一直在变化」。根因不是页签
    /// 本身，而是窗口：早先窗口高度是固定的 480，短页的内容被**垂直居中**
    /// 在那个高度里，于是每一页的页签条离窗口顶部的距离都不一样。
    /// 现在窗口高度贴着内容、内容钉在容器顶——两者一起断言：
    ///   · 内容顶 == 容器顶（不存在居中的余量）；
    ///   · 窗口高 == 内容高 + 内边距（不是永远 480）。
    @MainActor
    func testSettingsWindowContentIsPinnedToTop() throws {
        let model = try makeModel()
        defer { SettingsWindow.close() }   // 关掉静态强引用的那张窗口
        var contentHeights: [CGFloat] = []
        for pane in SettingsView.Pane.allCases {
            SettingsWindow.close()
            SettingsWindow.open(model: model, pane: pane)
            for _ in 0..<20 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }

            guard let window = SettingsWindow.controller?.window,
                  let content = window.contentView else {
                return XCTFail("\(pane.rawValue)：没拿到设置窗口")
            }
            // 容器只有一个子视图（那一页的 hosting view），看它有没有贴顶。
            guard let host = content.subviews.first else {
                return XCTFail("\(pane.rawValue)：容器里没有内容")
            }
            let gapFromTop = content.bounds.maxY - host.frame.maxY
            XCTAssertEqual(gapFromTop, 0, accuracy: 0.5,
                           "\(pane.rawValue) 页的内容没贴窗口顶（差 \(gapFromTop)）"
                           + "——一居中，页签条就会随窗口高度浮动")
            // 窗口要**正好**是这个内容的高度（多一分是留白、少一分是裁切）。
            XCTAssertEqual(content.bounds.height, host.frame.height, accuracy: 0.5,
                           "\(pane.rawValue)：窗口高 \(content.bounds.height) 与内容高 "
                           + "\(host.frame.height) 不符——窗口没有跟着当前页走")
            contentHeights.append(host.frame.height)

            XCTAssertEqual(host.frame.width, DS.settingsWidth, accuracy: 1,
                           "\(pane.rawValue) 页内容宽度应钉在设置窗口宽度上")
        }
        // 高度不许都一样：都一样说明窗口没跟着内容改（还是那个固定 480）。
        XCTAssertGreaterThan(Set(contentHeights.map { Int($0) }).count, 1,
                             "各页内容高度完全相同（\(contentHeights)）——"
                             + "说明窗口高度没有跟着当前页的内容走")
    }

    /// 下拉控件的宽度**不随选中值变**。
    ///
    /// 这条护的是用户实测的「下拉列表框宽度/对齐不稳」：`PopupLabel` 早先用
    /// `.frame(maxWidth: 340)`，同一控件量出来 93pt（`0x96f`）/ 190pt
    /// （`iTerm2 Solarized Dark`）两种宽度——短值缩回去、长值撑开，
    /// 旁边那一列色块跟着左右挪，两个下拉之间也左右错开。
    /// 修法是给它**定宽**（默认 240，最长的配色名也放得下）。
    @MainActor
    func testPopupLabelWidthIsIndependentOfTitle() throws {
        let titles = ["0x96f", "（不设置）", "iTerm2 Solarized Dark",
                      "Solarized Dark Higher Contrast"]
        var widths: [CGFloat] = []
        for title in titles {
            let host = NSHostingView(rootView: PopupLabel(title: title))
            host.layoutSubtreeIfNeeded()
            widths.append(host.fittingSize.width)
        }
        XCTAssertEqual(Set(widths.map { Int($0.rounded()) }).count, 1,
                       "同一个下拉换了值就换了宽度（\(zip(titles, widths).map { "\($0)=\($1)" })）"
                       + "——这一行会随选中项伸缩，旁边的色块跟着左右挪")
        // 最长的配色名不能被截：定宽要容得下它。
        XCTAssertGreaterThanOrEqual(widths[0], 234,
                                    "定宽 \(widths[0]) 放不下最长的配色名"
                                    + "（Solarized Dark Higher Contrast ≈ 234pt）")
    }

    /// 每一页都装进 NSHostingView 并强制布局：`Menu` 的 label、
    /// `@ViewBuilder` 分支、`@State` 初值都在这一步执行——写错会在这里崩。
    /// 顺带断言页面确实画出了东西（不是一片空）。
    @MainActor
    func testEveryPaneRendersNonEmpty() throws {
        let model = try makeModel()
        for pane in SettingsView.Pane.allCases {
            let view = SettingsView(model: model, initialPane: pane)
            let host = NSHostingView(rootView: view)
            host.frame = NSRect(x: 0, y: 0, width: DS.settingsWidth,
                                height: pane.height)
            host.layoutSubtreeIfNeeded()
            let size = host.fittingSize
            XCTAssertGreaterThan(size.width, 0, "\(pane.rawValue) 页宽度为 0")
            XCTAssertGreaterThan(size.height, 0, "\(pane.rawValue) 页高度为 0")
            XCTAssertEqual(size.width, DS.settingsWidth, accuracy: 2,
                           "\(pane.rawValue) 页宽度应钉在设置窗口的内容宽度上")

            // 抓一张帧：控件（文字/图标）必须真的画出来了——全空说明
            // 视图树没搭起来（或整页被裁掉了）。
            guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
                return XCTFail("\(pane.rawValue) 拿不到位图缓存")
            }
            host.cacheDisplay(in: host.bounds, to: rep)
            var nonWhite = 0
            for y in stride(from: 0, to: rep.pixelsHigh, by: 3) {
                for x in stride(from: 0, to: rep.pixelsWide, by: 3) {
                    guard let c = rep.colorAt(x: x, y: y)?
                        .usingColorSpace(.deviceRGB) else { continue }
                    let luma = c.redComponent * 0.3 + c.greenComponent * 0.59
                        + c.blueComponent * 0.11
                    if luma < 0.75 { nonWhite += 1 }
                }
            }
            XCTAssertGreaterThan(nonWhite, 20,
                                 "\(pane.rawValue) 页画出来几乎是空白"
                                 + "（非白像素 \(nonWhite)）——视图树可能没搭起来")
        }
    }

    // MARK: - 定高与内容对表

    /// 每页的定高要**对表内容**：够放（不裁、不靠滚动藏内容），也不留
    /// 大空白。两个真实翻车现场：外观页删掉「顶部标签条」一行后定高
    /// 640 没跟着收，底部空出 145pt（用户实测报「下面空很多」）；键盘页
    /// 420 装不下内容，底部 91pt 一直藏在滚动里。
    ///
    /// 量法：宿主按理想高渲染、离屏抓帧（未画到处全透明），自顶向下扫
    /// 最后一条有内容的像素行，`留白 = 窗口高 - 内容底`。下界防裁切
    /// （裁切时内容/滚动条画到窗口底，留白趋 0），上界防大空白。数值
    /// 实测而来：增删设置行后按 `Pane.height` 注释重新对表。
    ///
    /// **上界为什么是 55 而不是 40**：留白是环境相关量——定高是写死的，
    /// 而行高跟着系统控件高度漂。同一份代码实测：本机 macOS 26 四页留白
    /// 21~24pt，CI 的 macos-15 runner 上「外观」页 49pt（页里控件多，累计
    /// 漂 ≈28pt）。按 40 会在老系统上常红，而这条真正要拦的是「删了一行
    /// 没收高度」（当年 145pt）：一行约 40~55pt，任何环境都还在 55 之外。
    /// 每页留白都打出来（`留白 外观=…`），对不上表时能看见是谁漂了多少。
    @MainActor
    func testPaneHeightsMatchContent() throws {
        let model = try makeModel()
        for pane in SettingsView.Pane.allCases {
            let view = SettingsView(model: model, initialPane: pane)
            let host = NSHostingView(rootView: view)
            host.frame = NSRect(x: 0, y: 0, width: DS.settingsWidth, height: 0)
            host.layoutSubtreeIfNeeded()
            // 理想高 = 页签条 + 分隔线 + 定高内容（窗口实际就这么高）。
            let h = host.fittingSize.height
            host.frame = NSRect(x: 0, y: 0, width: DS.settingsWidth, height: h)
            host.layoutSubtreeIfNeeded()
            guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
                return XCTFail("\(pane.rawValue) 拿不到位图缓存")
            }
            host.cacheDisplay(in: host.bounds, to: rep)
            let scale = CGFloat(rep.pixelsWide) / host.bounds.width
            var lastRow = -1
            if let data = rep.bitmapData {
                let bpp = rep.bitsPerPixel / 8
                let bpr = rep.bytesPerRow
                for y in 0..<rep.pixelsHigh {
                    for x in stride(from: 0, to: rep.pixelsWide, by: 2) {
                        let p = data + y * bpr + x * bpp
                        if p[0] > 8 || p[1] > 8 || p[2] > 8 || p[3] > 8 {
                            lastRow = y          // 这行有内容；y 递增，直接记
                            break
                        }
                    }
                }
            }
            XCTAssertGreaterThanOrEqual(lastRow, 0, "\(pane.rawValue) 页扫不到任何内容")
            let gap = h - CGFloat(lastRow + 1) / scale
            // 归因：留白是**环境相关量**（控件高度随 macOS 版本漂），CI 与本机
            // 对不上时必须看得见是谁漂了多少，而不是只看到一个红。
            print("留白 \(pane.rawValue)=\(gap)pt 高=\(h) scale=\(scale)")
            XCTAssertGreaterThanOrEqual(gap, 2,
                "\(pane.rawValue) 页内容顶到窗口底（留白 \(gap)pt）——定高装不下，"
                + "底部藏在滚动里；见 Pane.height 注释重新对表")
            XCTAssertLessThanOrEqual(gap, 55,
                "\(pane.rawValue) 页底部留白 \(gap)pt——定高远大于内容；"
                + "见 Pane.height 注释重新对表")
        }
    }

    // MARK: - 外观页的配色色块（用户实测：主题预览没显示）

    /// 下拉**右边**那块配色色块真的画出来了。
    ///
    /// 用户实测报的「主题预览没正常显示」：数据面是好的——`themeFileText`
    /// 读得到、`ThemePreview` 解析得出颜色——但色块**画不出来**。根因是
    /// `ThemeSwatch` 被塞进了 `Menu` 的 **label**：label 会被 AppKit 收编成
    /// 按钮内容，只认**一个**视图，多给的兄弟视图（色块 + 八色条）整个被吃掉
    /// （只剩 `PopupLabel`）。修法是把色块挪成 `Menu` 的**兄弟**：
    /// `HStack { Menu {…} label: { PopupLabel }; ThemeSwatch }`。
    ///
    /// 断言直接数**色块背景色**的像素：选了一套纯红底的配色时，页面上必须
    /// 出现成片的红像素；不选配色时（对照）红像素必须几乎为零——证明判据
    /// 认的是色块本身，不是页面里别的红色。这条同时护住「色块不能回 label」。
    @MainActor
    func testThemeSwatchRendersBesidePicker() throws {
        let dir = try tempDir()
        // 资源目录：只放一套**纯红底**配色，色块一出就是一片红。
        let res = dir + "/ghostty"
        try FileManager.default.createDirectory(atPath: res + "/themes",
                                                withIntermediateDirectories: true)
        try "background = #ff0000\nforeground = #ffffff\n"
            .write(toFile: res + "/themes/Reds", atomically: true, encoding: .utf8)
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                             encoding: .utf8)
        let conf = dir + "/ghostty.conf"

        func redPixels(themeLine: String) throws -> (red: Int, total: Int) {
            try themeLine.write(toFile: conf, atomically: true, encoding: .utf8)
            let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts",
                                 settings: SettingsStore(terminalConfigPath: conf,
                                                         resourcesPath: res))
            XCTAssertEqual(model.availableThemes, ["Reds"], "主题清单应只有这一套")
            let view = SettingsView(model: model, initialPane: .appearance)
            let host = NSHostingView(rootView: view)
            host.frame = NSRect(x: 0, y: 0, width: DS.settingsWidth, height: 420)
            host.layoutSubtreeIfNeeded()
            guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
                throw XCTSkip("拿不到位图缓存（环境限制）")
            }
            host.cacheDisplay(in: host.bounds, to: rep)   // 先抓帧，再取 cgImage
            guard let cg = rep.cgImage else {
                throw XCTSkip("缓存位图没有 CGImage")
            }
            let w = cg.width, h = cg.height
            var buf = [UInt8](repeating: 0, count: w * h * 4)
            buf.withUnsafeMutableBytes { raw in
                guard let ctx = CGContext(
                    data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                    bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return }
                ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
            }
            var red = 0
            var i = 0
            while i + 3 < buf.count {
                // 纯红底（255,0,0）：只认「红明显压过绿蓝」的像素。
                if buf[i] > 150 && Int(buf[i]) > Int(buf[i + 1]) + 60
                    && Int(buf[i]) > Int(buf[i + 2]) + 60 { red += 1 }
                i += 4
            }
            return (red, w * h)
        }

        let selected = try redPixels(themeLine: "theme = \"Reds\"\n")
        let none = try redPixels(themeLine: "# 没有 theme 行\n")
        XCTAssertGreaterThan(selected.red, 200,
                             "选了纯红底配色，色块却没画出来（红像素 \(selected.red)）"
                             + "——色块多半又被塞回 Menu 的 label 被 AppKit 吃掉了")
        XCTAssertGreaterThan(selected.red, none.red * 10,
                             "没选配色时不应有红像素（有 \(none.red) 个）——"
                             + "判据得认的是色块，不是页面里别的红色")
    }
}

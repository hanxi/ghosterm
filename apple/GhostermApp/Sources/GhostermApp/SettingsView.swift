// SettingsView — 设置窗口里的那一页（Mac 的「偏好设置」形态）。
//
// 取法参照 iGhostVT 的 `Interface/Settings/Mac/`（用户指定的参照实现，
// 见 apple/README.md）：**顶部一排带图标的页签**，选中的页用强调色底，
// 下面就是那一页的内容；窗口自己就是设置，所以**没有「完成」按钮**
// ——直接关窗（⌘W / 红绿灯 / Esc）。
//
// **页签条的位置在每一页都一样**：窗口高度 = 页签条 + 那一页的高度，
// 两者都从顶部开始排（见 SettingsWindow）。所以切页时页签图标不会动。
import GhostermAppModel
import SwiftUI

/// 当前选中的页。**放在窗口层**（而不是视图的 `@State`）：
/// 菜单 ⌘, 要能把已经开着的窗口切到某一页（`SettingsWindow.open(pane:)`），
/// 而那发生在视图之外。
@MainActor
final class SettingsSelection: ObservableObject {
    @Published var pane: SettingsView.Pane

    init(pane: SettingsView.Pane) { self.pane = pane }
}

struct SettingsView: View {
    @ObservedObject var model: AppModel
    /// 页签由窗口持有（见上），视图只读写它。
    @ObservedObject var selection: SettingsSelection
    @Environment(\.dismiss) private var dismiss

    init(model: AppModel, initialPane: SettingsView.Pane = .general) {
        self.model = model
        _selection = ObservedObject(wrappedValue: SettingsSelection(pane: initialPane))
    }

    init(model: AppModel, selection: SettingsSelection) {
        self.model = model
        _selection = ObservedObject(wrappedValue: selection)
    }

    private var pane: SettingsView.Pane { selection.pane }

    enum Pane: String, CaseIterable, Identifiable {
        case general = "通用"
        case appearance = "外观"
        case keyboard = "键盘"
        case configuration = "配置文件"

        var id: String { rawValue }

        /// 图标**照抄 iGhostVT 的 MacSettingsPane**（同一套符号名，
        /// 让熟悉它的人一眼找得到同一项）。
        var symbol: String {
            switch self {
            case .general: return "gearshape"
            case .appearance: return "paintpalette"
            case .keyboard: return "keyboard"
            case .configuration: return "doc.text"
            }
        }

        /// 这一页的高度（pt）。窗口的高度 = 页签条 + 它。
        ///
        /// **每一页都要给**（包括短页）：短页给 nil 的话，窗口高度就由
        /// 「这一页有多少字」决定，切页时窗口会忽高忽低——而页签条是钉在
        /// 窗口顶上的，于是用户看到的就是图标位置一直在变。短页取一个
        /// 够放内容的定高，比「贴着内容走」稳（iGhostVT 的 Mac 设置也是
        /// 定高窗口，`fixedHeight` 那套）。
        ///
        /// 数值是**对表内容量出来的**（增删设置行后要跟着改）：底部留白
        /// ≈ 竖直内缩（Padding.l=16）+ 小余量，由
        /// `SettingsUITests.testPaneHeightsMatchContent` 像素级钉住——
        /// 留白超过容差即红（「外观页下面空一大截」就是这么漏出去又被
        /// 抓回来的：删了「顶部标签条」一行，640 没跟着收；键盘页则相反，
        /// 420 装不下内容，底部 91pt 一直藏在滚动里）。
        var height: CGFloat {
            switch self {
            case .general: return 320
            case .appearance: return 560
            case .keyboard: return 535
            case .configuration: return 445
            }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            panePicker
            Divider()
            paneContent
        }
        .frame(width: DS.settingsWidth)
        // Esc 关窗（macOS 上「关掉这个面板」的常规手势）。不带「完成」
        // 按钮，所以这条是键盘用户唯一的出口，必须给。
        .background(
            Button("") { closeWindow() }
                .keyboardShortcut(.cancelAction)
                .opacity(0).frame(width: 0, height: 0)
                .accessibilityHidden(true)
        )
    }

    /// 页签下方的内容：**定高**，装不下的自己滚（长页）或留白（短页）。
    ///
    /// 早先短页是「由内容自己撑」，于是窗口高度跟着文字多少变——页签条
    /// 就被顶得上下跳。定高之后每一页的页签条都在同一个 y 上。
    private var paneContent: some View {
        ScrollView {
            paneBody
        }
        .frame(height: pane.height)
    }

    private var paneBody: some View {
        Group {
            switch pane {
            case .general: GeneralPane(model: model)
            case .appearance: AppearancePane(model: model)
            case .keyboard: KeyboardPane()
            case .configuration: ConfigurationPane(model: model)
            }
        }
        .padding(.horizontal, DS.Padding.xl)
        .padding(.vertical, DS.Padding.l)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func closeWindow() {
        // 关的是**设置窗口自己**（不是「当前最前的窗口」——那样一旦焦点
        // 飘到主窗口就会把它关掉）。
        if let window = SettingsWindow.controller?.window {
            window.close()
        } else {
            dismiss()
        }
    }

    /// 页签条：图标在上、名字在下，选中项带强调色底。
    private var panePicker: some View {
        HStack(spacing: DS.Padding.xs) {
            ForEach(Pane.allCases) { p in
                Button {
                    selection.pane = p
                } label: {
                    VStack(spacing: DS.Padding.xs) {
                        Image(systemName: p.symbol)
                            .font(DS.Font.symbol)
                        Text(p.rawValue)
                            .font(DS.Font.caption)
                    }
                    .frame(width: 76, height: 54)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(pane == p ? Color.accentColor : Color.primary)
                .background(
                    RoundedRectangle(cornerRadius: DS.Radius.control)
                        .fill(pane == p ? Color.accentColor.opacity(0.12)
                                        : Color.clear)
                )
                .help(p.rawValue)
                .accessibilityLabel(p.rawValue)
                .accessibilityAddTraits(pane == p ? [.isSelected] : [])
            }
            Spacer()
        }
        .padding(.horizontal, DS.Padding.m)
        .padding(.vertical, DS.Padding.s)
    }
}

// MARK: - 通用

/// 通用：默认命令行解释器（下拉）+ 新建标签页的记忆行为，另有当前终端的
/// 配色集是否随包提供的诚实提示。
private struct GeneralPane: View {
    @ObservedObject var model: AppModel

    /// 下拉可选的 shell。nil = 还没读到（`/etc/shells`）。
    @State private var availableShells: [String]?
    @State private var isEditingCustomShell = false
    @FocusState private var customFocused: Bool

    /// 下拉里显示自定义路径的输入框吗：正在编辑，或存着一个不在列表里的
    /// 路径（用户手填过）。`availableShells == nil` 时不显示——不能因为
    /// 「还没读到」就误报成自定义。
    private var showsCustomPath: Bool {
        ShellCatalog.showsCustomPath(model.preferences.defaultShell,
                                     available: availableShells,
                                     isEditing: isEditingCustomShell)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            SettingsRow("默认命令行解释器") {
                Menu {
                    shellMenuItems
                } label: {
                    PopupLabel(title: ShellCatalog.title(
                        of: model.preferences.defaultShell,
                        isCustom: showsCustomPath && !isEditingCustomShell
                            && !(availableShells ?? []).isEmpty
                            && !availableShells!.contains(model.preferences.defaultShell)))
                }
                .accessibilityLabel("默认命令行解释器")
            } details: {
                VStack(alignment: .leading, spacing: DS.Padding.s) {
                    if showsCustomPath {
                        TextField("可执行文件路径，如 /opt/homebrew/bin/bash",
                                  text: Binding(
                                    get: { model.preferences.defaultShell },
                                    set: { model.preferences.defaultShell = $0 }))
                            .textFieldStyle(.roundedBorder)
                            .focused($customFocused)
                            .frame(maxWidth: 340)
                            .onSubmit { isEditingCustomShell = false }
                    }
                    SettingsNote("选择「自动」以使用登录 shell。若未列在上方，"
                                 + "请输入可执行文件的自定义路径。")
                    if let shells = availableShells, shells.isEmpty {
                        SettingsNote("系统没有登记任何 shell（/etc/shells 为空或读不到）"
                                     + "——只剩「自动」与「自定义…」。")
                    }
                }
            }

            Divider()

            Text("会话").font(DS.Font.labelEmphasis)
            SettingsRow("新建标签页") {
                Checkbox("记住最近使用的目录",
                         isOn: Binding(
                            get: { model.preferences.rememberDirectories },
                            set: { model.preferences.rememberDirectories = $0 }))
            } details: {
                SettingsNote("新建标签页会先列出终端当前所在的目录，再列出它们去过的目录。"
                             + "关闭后会清除后一份列表，并不再记录。")
            }

            SettingsRow("启动") {
                Checkbox("启动时新建会话", isOn: Binding(
                    get: { model.preferences.shellOnLaunch },
                    set: { model.preferences.shellOnLaunch = $0 }))
            } details: {
                SettingsNote("开启后，启动应用时若没有可恢复的会话，会新建一个。"
                             + "关闭后，应用启动时不打开任何标签页。")
            }

            SettingsRow("标签页") {
                Checkbox("启动时锁定标签页", isOn: Binding(
                    get: { model.preferences.lockTabsOnLaunch },
                    set: { model.preferences.lockTabsOnLaunch = $0 }))
            } details: {
                SettingsNote("避免刚打开应用就误触输入。")
            }

            SettingsNote("会话随标签页存在：标签页关掉，会话就结束。"
                         + "这里**没有**「保持会话运行」那一项——那个要靠常驻后台"
                         + "助手（launchd/XPC）让会话活过应用退出，我们不做常驻 helper。")
        }
        .onAppear {
            if availableShells == nil {
                availableShells = ShellCatalog.availableShells()
            }
        }
    }

    /// shell 下拉：自动 / 系统登记的每个 shell / 自定义…。
    /// 用 Button 而不是 Picker：Menu 里放 Picker 会变成一个同名子菜单。
    @ViewBuilder
    private var shellMenuItems: some View {
        Button {
            model.preferences.defaultShell = ""
            isEditingCustomShell = false
        } label: {
            checkItem("自动", checked: !showsCustomPath
                      && model.preferences.defaultShell.isEmpty)
        }
        Divider()
        ForEach(availableShells ?? [], id: \.self) { path in
            Button {
                model.preferences.defaultShell = path
                isEditingCustomShell = false
            } label: {
                checkItem(path, checked: !showsCustomPath
                          && model.preferences.defaultShell == path)
            }
        }
        Divider()
        Button {
            isEditingCustomShell = true
            DispatchQueue.main.async { customFocused = true }
        } label: {
            checkItem("自定义…", checked: isEditingCustomShell)
        }
    }

    /// 菜单项：选中项带对勾（`Menu` 里不能自己画 Label 的选中态，
    /// 用 systemImage 显式给）。
    @ViewBuilder
    private func checkItem(_ title: String, checked: Bool) -> some View {
        if checked {
            Label(title, systemImage: "checkmark")
        } else {
            Text(title)
        }
    }
}

// MARK: - 外观

/// 外观：界面模式（浅色 / 深色 / 跟随系统）+ 界面字号 + 终端字号 +
/// 浅色 / 深色两档配色。
private struct AppearancePane: View {
    @ObservedObject var model: AppModel

    @State private var interfaceStep: Int = InterfaceTextSize.defaultStep
    @State private var terminalFontSize: Double = 13
    @State private var errorText: String?

    // 窗格外观两项。用 @AppStorage 而不是 @State + 手写回存：主窗口那边
    // 读的是同一批键，改完要**当场**生效（用户不该为了看效果重开窗口）。
    @AppStorage(PaneGap.key) private var paneGap = PaneGap.standard
    @AppStorage(PaneTitleBar.visibleKey)
    private var paneTitleBarVisible = PaneTitleBar.visibleDefault

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            SettingsRow("外观") {
                Menu {
                    ForEach(AppearancePreference.allCases) { choice in
                        Button {
                            model.appearance = choice
                        } label: {
                            if choice == model.appearance {
                                Label(choice.title, systemImage: "checkmark")
                            } else {
                                Text(choice.title)
                            }
                        }
                    }
                } label: {
                    PopupLabel(title: model.appearance.title)
                }
                .accessibilityLabel("外观")
                .accessibilityValue(model.appearance.title)
            } details: {
                SettingsNote("界面与终端配色一起跟着这一项走——"
                             + "终端那边是 Ghostty 的 `theme` 条件值，"
                             + "深浅切换时引擎自己选边。")
            }

            Divider()

            SettingsRow("标签位置") {
                Menu {
                    ForEach(TabPlacement.allCases) { placement in
                        Button(placement.title) { model.preferences.tabPlacement = placement }
                    }
                } label: {
                    PopupLabel(title: model.preferences.tabPlacement.title)
                }
                .accessibilityLabel("标签位置")
                .accessibilityValue(model.preferences.tabPlacement.title)
            }
            SettingsRow("侧边标签") {
                Checkbox("精简模式", isOn: $model.preferences.compactSidebar)
                    .disabled(model.preferences.tabPlacement != .sidebar)
            }

            Divider()

            SettingsRow("终端") {
                HStack(spacing: DS.Padding.m) {
                    ValueText(text: points(terminalFontSize))
                    Stepper("终端", value: $terminalFontSize,
                            in: TerminalFontSize.range, step: 0.5)
                        .labelsHidden()
                        .onChange(of: terminalFontSize) { applyTerminalFont(size: $0) }
                }
            } details: {
                SettingsNote("新开的终端用这个字号；已经开着的标签页保持它当时的字号。")
            }

            SettingsRow("界面") {
                HStack(spacing: DS.Padding.m) {
                    ValueText(text: InterfaceTextSize.percent(step: interfaceStep))
                    Stepper("界面", value: $interfaceStep,
                            in: InterfaceTextSize.steps)
                        .labelsHidden()
                        .onChange(of: interfaceStep) { step in
                            UserDefaults.standard.set(step, forKey: InterfaceTextSize.key)
                        }
                }
            } details: {
                SettingsNote("界面字号缩放每一个标签与控件（终端里的字符不走它）。")
            }

            Divider()

            SettingsRow("浅色配色") { slotPicker(.light) }
            SettingsRow("深色配色") { slotPicker(.dark) } details: {
                SettingsNote(themesNote)
            }

            Divider()

            // 分屏那块地方长什么样。两项都能调到最保守的一端——调满之后
            // 屏幕上就完全回到加这套窗格卡片之前的样子。
            SettingsRow("窗格间距") {
                Menu {
                    ForEach(PaneGap.steps, id: \.self) { step in
                        Button {
                            paneGap = step
                        } label: {
                            if step == PaneGap.resolved(paneGap) {
                                Label(PaneGap.title(of: step), systemImage: "checkmark")
                            } else {
                                Text(PaneGap.title(of: step))
                            }
                        }
                    }
                } label: {
                    PopupLabel(title: PaneGap.title(of: PaneGap.resolved(paneGap)))
                }
                .accessibilityLabel("窗格间距")
                .accessibilityValue(PaneGap.title(of: PaneGap.resolved(paneGap)))
            } details: {
                SettingsNote("格与格、格与窗口边之间留白。调到「无」就是贴边分屏："
                             + "不画圆角，只剩分隔线。")
            }

            SettingsRow("窗格标题栏") {
                Checkbox("显示", isOn: $paneTitleBarVisible)
            } details: {
                SettingsNote("每格顶上一条：状态点、进程名、目录。"
                             + "右侧的操作按钮只画在当前那格。")
            }

            if let errorText {
                Text(errorText).font(DS.Font.detail).foregroundStyle(.red)
            }
        }
        .onAppear(perform: load)
    }

    private var themesNote: String {
        let themes = model.availableThemes
        if themes.isEmpty {
            return "配色集未随包提供（主题列表为空）。开发态直接跑可执行文件时"
                + "属正常——打包后的 .app 里有 Contents/Resources/ghostty/themes。"
        }
        return "配色来自 Ghostty 配色集（随包 \(themes.count) 套），"
            + "应用到每一个窗口的每一个标签页。只选一档时另一档跟着它走。"
    }

    /// 一档配色的下拉：不设置 / 每套配色，右边跟着当前配色的色块。
    ///
    /// **色块被 AppKit 吃过两次，两次都不许再放回去**：
    ///   1. 色块**不能进菜单条目**：`Menu` 的每一条会被压成一条真 `NSMenuItem`，
    ///      里面塞 `HStack` / `ZStack` / 自绘 `Canvas` 这种普通视图撑不住——实测
    ///      把 `ThemeSwatch`（色块 + 八色条 + `$_` 提示符）放进条目里，下拉里每
    ///      一行都塌成那个字面量 `$_` 加几个色块，**配色名整个丢掉**（用户实测
    ///      截图）。
    ///   2. 色块也**不能进 `Menu` 的 label**：label 会被 AppKit 收编成「按钮」
    ///      的内容，多给一个兄弟视图就塌——实测把 label 写成
    ///      `HStack { PopupLabel; ThemeSwatch }` 时，**色块整个不画**（用户报的
    ///      「主题预览没正常显示」就是这个：`ThemePreview` 数据明明读到了，
    ///      画不出来的是布局）。`label` 只能放 `PopupLabel` 一个视图。
    ///
    /// 所以色块放在 `Menu` **外面**、当它的兄弟：`HStack { Menu {…} label: {…};
    /// ThemeSwatch }`。iGhostVT 的 Mac 版对第 1 条的注释原话是 AppKit draws no
    /// image on a Catalyst popup menu's items, so they cannot be in it —— 它也
    /// 是把色块挪到按钮**旁边**。条目里只留名字 + 对勾。
    @ViewBuilder
    private func slotPicker(_ slot: ThemeSlot) -> some View {
        let selected = model.themeSelection.effective(slot)
        // 色块是 Menu 的**兄弟**（见上：进 label 会被 AppKit 吃掉）。
        HStack(spacing: DS.Padding.s) {
            Menu {
                Button {
                    setTheme(slot: slot, name: nil)
                } label: {
                    if selected == nil {
                        Label("不设置", systemImage: "checkmark")
                    } else {
                        Text("不设置")
                    }
                }
                Divider()
                ForEach(model.availableThemes, id: \.self) { name in
                    Button {
                        setTheme(slot: slot, name: name)
                    } label: {
                        if selected == name {
                            Label(name, systemImage: "checkmark")
                        } else {
                            Text(name)
                        }
                    }
                }
            } label: {
                // label 里**只放这一个** `PopupLabel`（多塞一个兄弟视图就塌，
                // 见上）。定宽 240（不是 `maxWidth`）：短值（`0x96f`）与长值
                // （`iTerm2 Solarized Dark`）宽度必须一样，否则这一整行
                // 会随选中项伸缩、色块跟着左右挪（用户实测）。
                PopupLabel(title: selected ?? "（不设置）")
            }
            .disabled(model.availableThemes.isEmpty)
            .accessibilityLabel(slot.title)
            .accessibilityValue(selected ?? "不设置")

            if let preview = preview(of: selected) {
                ThemeSwatch(preview: preview)
            }
        }
    }

    /// 选中那套配色的迷你预览（下拉**旁边**那一小块，见 `slotPicker`）。
    ///
    /// 只读**当前选中的那一套**：一次一个 ~1KB 的小文件，而且只在按钮重画时读。
    /// 早先在 `load()` 里把 600 多套全读进 `@State` 缓存，是为了给**每个菜单
    /// 条目**画色块——那条路已经走不通（见 `slotPicker` 的说明），缓存也就
    /// 没必要了。
    private func preview(of name: String?) -> ThemePreview? {
        guard let name, let text = model.settings.themeFileText(named: name) else {
            return nil
        }
        return ThemePreview(text: text)
    }

    private func setTheme(slot: ThemeSlot, name: String?) {
        var selection = model.themeSelection
        switch slot {
        case .light: selection.light = name
        case .dark: selection.dark = name
        }
        do {
            try model.applyThemeSelection(selection)
            errorText = nil
        } catch {
            errorText = "\(error)"
        }
    }

    private func load() {
        interfaceStep = UserDefaults.standard.object(
            forKey: InterfaceTextSize.key) as? Int ?? InterfaceTextSize.defaultStep
        terminalFontSize = model.terminalSetting("font-size")
            .flatMap(Double.init) ?? 13
    }

    /// 终端字号是 Ghostty 配置里的事（`font-size`）——写文件再让引擎重载。
    private func applyTerminalFont(size: Double) {
        do {
            try model.applyTerminalSettings(
                [("font-size", String(format: "%g", size))])
            errorText = nil
        } catch {
            errorText = "\(error)"
        }
    }

    private func points(_ size: Double) -> String {
        "\(String(format: "%g", size)) pt"
    }
}

// MARK: - 键盘

/// 键盘：本应用快捷键一览。
///
/// 这张表**不是**说明书：`SettingsUITests` 会拿它跟真正装上的主菜单
/// （`AppMenu`）双向对拍——标题与按键必须一一对上。所以「列了但按不出来」
/// 会被断言拦住，不会悄悄过期。
private struct KeyboardPane: View {
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            ForEach(ShortcutCatalog.groups, id: \.title) { group in
                Text(group.title)
                    .font(DS.Font.labelEmphasis)
                    .foregroundStyle(.secondary)
                    .padding(.top, DS.Padding.s)
                ForEach(group.items, id: \.title) { item in
                    HStack(spacing: DS.Padding.m) {
                        Text(item.title)
                        Spacer(minLength: DS.Padding.m)
                        Text(item.keys)
                            .font(DS.Font.code)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.horizontal, DS.Padding.m)
                    .padding(.vertical, DS.Padding.xs + 2)
                    .background(RoundedRectangle(cornerRadius: DS.Radius.s)
                        .fill(Color.secondary.opacity(0.08)))
                    .accessibilityElement(children: .combine)
                }
            }
            SettingsNote("终端里正在跑的程序优先：这些组合带 ⌘，由菜单接走；"
                         + "其余按键（含 Esc 与 Ctrl 组合）原样送给终端。"
                         + "重绑暂未开放——那要先有「关掉某个快捷键就让那个键"
                         + "回到终端」的完整机制（菜单项撤下 + 键位接管），"
                         + "现在只做得到把键拦在菜单这一层。")
                .padding(.top, DS.Padding.s)
        }
    }
}

// MARK: - 配置文件

/// 配置文件：ssh_config / known_hosts / 终端配置三个路径，以及**自定义
/// 配置行**与**渲染出来的完整配置**（照抄 iGhostVT 的 Ghostty
/// Configuration 页：上一半可编辑，下一半是只读的成品，一眼能对上）。
private struct ConfigurationPane: View {
    @ObservedObject var model: AppModel

    /// 用户自己写的那些行（`ghostty.conf` 原文）。
    @State private var customLines = ""
    @State private var loaded = false
    @State private var statusText: String?
    @State private var errorText: String?

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            pathRow("ssh_config", model.configPath)
            pathRow("known_hosts", model.knownHostsPath)
            pathRow("终端配置", model.settings.terminalConfigPath)

            Divider()

            Text("自定义配置").font(DS.Font.labelEmphasis)
            TextEditor(text: $customLines)
                .font(DS.Font.code)
                .frame(height: 110)
                .padding(DS.Padding.xs)
                .background(RoundedRectangle(cornerRadius: DS.Radius.s)
                    .fill(Color.secondary.opacity(0.08)))
                .overlay(RoundedRectangle(cornerRadius: DS.Radius.s)
                    .strokeBorder(Color.secondary.opacity(0.2)))
                .accessibilityLabel("自定义配置")

            HStack {
                SettingsNote("按 Ghostty 语法写；只有**新开的**标签页会读到改动。")
                Spacer()
                if let statusText {
                    Text(statusText).font(DS.Font.detail).foregroundStyle(.secondary)
                }
                Button("保存") { save() }
                    .disabled(!loaded)
            }
            if let errorText {
                Text(errorText).font(DS.Font.detail).foregroundStyle(.red)
                    .textSelection(.enabled)
            }

            Divider()

            Text("当前生效的配置").font(DS.Font.labelEmphasis)
            SettingsNote("这是引擎实际读到的那份文件（改上面那段之后、新开的标签页用它）。")
            ConfigurationFileView(contents: model.terminalConfigText)
        }
        .onAppear(perform: load)
    }

    private func pathRow(_ label: String, _ path: String) -> some View {
        HStack(spacing: DS.Padding.m) {
            Text(label).frame(width: 110, alignment: .leading)
            Text(path)
                .font(DS.Font.code)
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 0)
            Button {
                NSWorkspace.shared.activateFileViewerSelecting(
                    [URL(fileURLWithPath: path)])
            } label: {
                Image(systemName: "arrow.up.right.circle.fill")
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.borderless)
            .help("在访达中显示")
            .accessibilityLabel("在访达中显示 \(label)")
        }
    }

    private func load() {
        customLines = model.terminalConfigText
        loaded = true
    }

    /// 保存：整份文本原样写回（**不做增量补丁**——用户在这块里写的任何
    /// 东西都是他的原文，逐字保真是铁律 #4 的精神在终端配置上的对应）。
    private func save() {
        do {
            try model.replaceTerminalConfig(customLines)
            errorText = nil
            statusText = "已保存"
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { statusText = nil }
        } catch {
            errorText = "\(error)"
            statusText = nil
        }
    }
}

// MARK: - 共享控件（照抄 iGhostVT 的 MacSettingsControls）

/// 一行的布局：标签在左（右对齐、定宽，行行对齐），控件紧随其右；
/// 说明与次级控件叠在控件**那一列**下面。
struct SettingsRow<Control: View, Details: View>: View {
    let label: String
    @ViewBuilder let control: () -> Control
    @ViewBuilder let details: () -> Details

    init(_ label: String,
         @ViewBuilder control: @escaping () -> Control,
         @ViewBuilder details: @escaping () -> Details) {
        self.label = label
        self.control = control
        self.details = details
    }

    /// 一个设置页里所有行共用的标签列宽（照抄 iGhostVT 的 150）。
    static var labelWidth: CGFloat { 150 }
    static var spacing: CGFloat { DS.Padding.m }

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            HStack(alignment: .center, spacing: Self.spacing) {
                Text(label)
                    .multilineTextAlignment(.trailing)
                    .frame(width: Self.labelWidth, alignment: .trailing)
                control()
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            VStack(alignment: .leading, spacing: DS.Padding.s) { details() }
                .padding(.leading, Self.labelWidth + Self.spacing)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

extension SettingsRow where Details == EmptyView {
    init(_ label: String, @ViewBuilder control: @escaping () -> Control) {
        self.init(label, control: control, details: { EmptyView() })
    }
}

/// 行下面的灰色说明。
struct SettingsNote: View {
    let text: String

    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(DS.Font.detail)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// 下拉按钮的样子：当前值 + 上下箭头，圆角填充。照抄 `MacPopupLabel`。
///
/// **宽度是给定的，不是「顺着内容长」**（踩过的坑）：用 `.frame(maxWidth:)`
/// 时这个控件会把可用宽度都吃掉、但**短值又缩回去**——于是「外观」页那两个
/// 配色下拉会随选中项忽宽忽窄（`0x96f` 比 `iTerm2 Solarized Dark` 窄
/// 约 100pt），旁边那一列色块也跟着左右挪；两个下拉之间还会左右错开。
/// 实测同一控件量出来 93 / 190 pt 两种宽度。
/// 设置页里的下拉都是**定宽**的（iGhostVT 的 `MacPopupLabel` 也走
/// `.frame(maxWidth: 340)`，但它的行里没有第二个控件，撑不出这个毛病）。
/// 断言见 `SettingsUITests.testPopupLabelWidthIsIndependentOfTitle`。
struct PopupLabel: View {
    let title: String
    /// 控件的固定宽度。默认 240：最长的配色名（`Solarized Dark Higher
    /// Contrast`，实测约 234pt）也放得下，不会被截。
    var width: CGFloat = 240

    var body: some View {
        HStack(spacing: DS.Padding.s) {
            Text(verbatim: title)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: DS.Padding.s)
            Image(systemName: "chevron.up.chevron.down")
                .imageScale(.small)
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
        }
        .padding(.horizontal, DS.Padding.m)
        .padding(.vertical, DS.Padding.xs + 2)
        .frame(width: width, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: DS.Radius.s)
            .fill(Color.secondary.opacity(0.12)))
        .contentShape(RoundedRectangle(cornerRadius: DS.Radius.s))
    }
}

/// 复选框：照抄 AppKit 的样子（小圆角方块，勾选时强调色 + 白勾），
/// 标题与方块是**同一个**点击目标。
///
/// 为什么不用 SwiftUI 的 Toggle：macOS 上 `Toggle` 默认画成开关
/// （`.checkbox` 样式也不是系统那种），与设置页其余控件不是一套。
struct Checkbox: View {
    let title: String
    @Binding var isOn: Bool

    init(_ title: String, isOn: Binding<Bool>) {
        self.title = title
        _isOn = isOn
    }

    var body: some View {
        Button { isOn.toggle() } label: {
            HStack(spacing: DS.Padding.s) {
                CheckboxBox(isOn: isOn)
                Text(title)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityValue(isOn ? "已勾选" : "未勾选")
    }
}

/// 复选框的方块本身（表格行自己就是点击目标时单用它）。
struct CheckboxBox: View {
    let isOn: Bool

    /// 14pt，AppKit 常规复选框的尺寸。
    private static let side: CGFloat = 14
    private let shape = RoundedRectangle(cornerRadius: 3.5, style: .continuous)

    var body: some View {
        ZStack {
            if isOn {
                shape.fill(Color.accentColor)
                Image(systemName: "checkmark")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundStyle(.white)
            } else {
                shape.fill(Color(nsColor: .textBackgroundColor))
                shape.strokeBorder(Color.primary.opacity(0.25), lineWidth: 0.75)
            }
        }
        .frame(width: Self.side, height: Self.side)
        .accessibilityHidden(true)
    }
}

/// 等宽数字的数值显示（让两个 Stepper 的数值列对齐，不随位数抖动）。
struct ValueText: View {
    let text: String

    var body: some View {
        Text(verbatim: text)
            .monospacedDigit()
            .lineLimit(1)
            .frame(width: 64, alignment: .trailing)
    }
}

/// 表格外观的框：文字底色 + 发丝边，行可以隔行染色（照抄
/// `MacTableFrame` / `MacTableStripe`）。
struct MacTableFrame<Content: View>: View {
    @ViewBuilder let content: () -> Content

    var body: some View {
        content()
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: DS.Radius.s, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: DS.Radius.s, style: .continuous)
                .strokeBorder(Color(nsColor: .separatorColor), lineWidth: 1))
    }
}

enum MacTableStripe {
    /// 隔行染色：偶数行透明，奇数行淡淡一层。
    static func color(_ index: Int) -> Color {
        index.isMultiple(of: 2) ? .clear : Color.secondary.opacity(0.07)
    }
}

/// 配置文件视图：标题条（文件名 + 行数 + 拷贝）+ 带行号的正文，
/// 横向可滚（长值不换行，读起来像编辑器）。照抄 iGhostVT 的
/// `ConfigurationFileView`。
struct ConfigurationFileView: View {
    let contents: String

    private var lines: [String] {
        var lines = contents.split(separator: "\n", omittingEmptySubsequences: false)
            .map(String.init)
        while lines.last?.isEmpty == true { lines.removeLast() }
        return lines
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: DS.Padding.s) {
                Image(systemName: "doc.text")
                    .font(DS.Font.captionEmphasis)
                    .foregroundStyle(.secondary)
                    .accessibilityHidden(true)
                Text(verbatim: "ghostty.conf")
                    .font(DS.Font.captionEmphasis)
                    .foregroundStyle(.secondary)
                Text("\(lines.count) 行")
                    .font(DS.Font.caption)
                    .foregroundStyle(Color.secondary.opacity(0.7))
                Spacer()
                Button {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(contents, forType: .string)
                } label: {
                    Label("拷贝", systemImage: "doc.on.doc")
                        .font(DS.Font.captionEmphasis)
                }
                .buttonStyle(.borderless)
                .foregroundStyle(Color.accentColor)
            }
            .padding(.horizontal, DS.Padding.m)
            .padding(.vertical, DS.Padding.s)
            .background(Color.secondary.opacity(0.10))

            if lines.isEmpty {
                Text(emptyNote)
                    .font(DS.Font.code)
                    .foregroundStyle(.secondary)
                    .padding(DS.Padding.m)
            } else {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(alignment: .top, spacing: DS.Padding.m) {
                        VStack(alignment: .trailing, spacing: 0) {
                            ForEach(lines.indices, id: \.self) { i in
                                Text(verbatim: "\(i + 1)")
                                    .foregroundStyle(Color.secondary.opacity(0.5))
                            }
                        }
                        .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 0) {
                            ForEach(lines.indices, id: \.self) { i in
                                ConfigurationLine(text: lines[i])
                            }
                        }
                    }
                    .font(DS.Font.code)
                    .padding(DS.Padding.m)
                    .textSelection(.enabled)
                }
            }
        }
    }

    private var emptyNote: String {
        "（空——全部使用引擎默认值）"
    }
}

/// 配置里的一行：`key = value` 时把键染成强调色（读起来像代码），
/// 不是那种形状（注释、空行）就原样画。
private struct ConfigurationLine: View {
    let text: String

    var body: some View {
        if let sep = text.range(of: " = ") {
            (Text(verbatim: String(text[..<sep.lowerBound]))
                .foregroundColor(.accentColor)
             + Text(verbatim: " = ").foregroundColor(.secondary)
             + Text(verbatim: String(text[sep.upperBound...])))
                .lineLimit(1)
                .fixedSize(horizontal: true, vertical: false)
        } else {
            Text(verbatim: text)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .fixedSize(horizontal: true, vertical: false)
        }
    }
}

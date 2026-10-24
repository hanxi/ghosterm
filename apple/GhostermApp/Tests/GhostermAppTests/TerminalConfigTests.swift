// TerminalConfigTests — 终端配置**真的进了引擎**（不是写了个文件就完事）。
//
// 为什么需要这条断言：设置页的「字体大小」这种控件最容易变成摆设——
// 文件写了、界面刷新了、用户看到的却还是老样子。这里用**可观测的引擎状态**
// 验证配置生效：字号变大 → 同样像素尺寸下网格的列数/行数应该变少。
// 这是不依赖屏幕录制权限的「渲染前一步」证据（像素级验证仍受权限限制，
// 见 RenderProbeTests 的口径）。
//
// 另覆盖 SettingsStore 的写侧语义：逐行保真合并、外部修改检测、备份。
import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest

final class TerminalConfigTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func tempDir() throws -> String {
        let dir = NSTemporaryDirectory() + "ghosterm-cfg-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        return dir
    }

    // MARK: - 配置真的生效

    /// 字号 → 网格密度：同一个像素尺寸下，字号大的网格必然更稀疏。
    /// 中间隔了一次 `gt_ghostty_config_get` 探针（配置加载失败会在这里红）。
    func testFontSizeChangesGridDensity() throws {
        let dir = try tempDir()
        let small = dir + "/small.conf"
        let large = dir + "/large.conf"
        try "font-size = 8\n".write(toFile: small, atomically: true, encoding: .utf8)
        try "font-size = 24\n".write(toFile: large, atomically: true, encoding: .utf8)

        let (smallCols, smallRows) = try grid(of: small)
        let (largeCols, largeRows) = try grid(of: large)

        XCTAssertGreaterThan(smallCols, largeCols,
            "字号 8 的列数应多于字号 24（得到 \(smallCols) vs \(largeCols)）——"
            + "配置若没进引擎，两者会相等")
        XCTAssertGreaterThan(smallRows, largeRows,
            "字号 8 的行数应多于字号 24（得到 \(smallRows) vs \(largeRows)）")
    }

    /// 同一个 app 上 reload 配置 → 已有 surface 调 applyConfig 后网格跟着变。
    /// 这条验证的是设置页「保存」之后的那一步（app 级更新不会自动落到
    /// 已有 surface，libghostty 的语义）。
    func testReloadConfigAppliesToExistingSurface() throws {
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "font-size = 8\n".write(toFile: conf, atomically: true, encoding: .utf8)

        let app = try loadConfig(conf)
        let surface = try TerminalSurface(
            app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let before = surface.gridSize.cols
        XCTAssertGreaterThan(before, 0)

        try "font-size = 24\n".write(toFile: conf, atomically: true, encoding: .utf8)
        XCTAssertNil(app.reloadConfig(path: conf), "重载合法配置不应报错")
        surface.setSize(width: 800, height: 400)
        let after = surface.gridSize.cols
        XCTAssertLessThan(after, before,
            "重载配置后已有终端面的网格应变稀疏（\(before) → \(after)）——"
            + "app 级更新应自动下发到所有 surface")
    }

    /// 无效配置必须**显式失败**（不静默回落成默认值——那样用户改了字体
    /// 却看到默认大小，无从查起）。
    func testInvalidConfigFailsExplicitly() throws {
        let dir = try tempDir()
        let conf = dir + "/bad.conf"
        try "font-size = 12\nthis-key-does-not-exist = 1\n"
            .write(toFile: conf, atomically: true, encoding: .utf8)

        XCTAssertThrowsError(try loadConfig(conf),
                             "含未知键的配置应显式失败") { error in
            let text = "\(error)"
            XCTAssertTrue(text.contains("配置无效") || text.contains("invalid"),
                          "错误串应说明是配置问题（得到 \(text)）")
        }
    }

    /// 加载配置失败时，**旧配置保持有效**（不半途换配置）。
    func testFailedReloadKeepsPreviousConfig() throws {
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "font-size = 8\n".write(toFile: conf, atomically: true, encoding: .utf8)
        let app = try loadConfig(conf)
        let surface = try TerminalSurface(
            app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let before = surface.gridSize.cols

        try "font-size = 24\nbogus-key = 1\n"
            .write(toFile: conf, atomically: true, encoding: .utf8)
        XCTAssertNotNil(app.reloadConfig(path: conf), "非法配置应报错")
        surface.setSize(width: 800, height: 400)
        XCTAssertEqual(surface.gridSize.cols, before,
                       "重载失败后网格不应改变（旧配置仍在生效）")
    }

    /// 读回单个键：这是设置页「回显」走的那条路（AppModel.terminalSetting
    /// → TerminalApp.float）。它踩过一个真缺陷：libghostty 按键的**字节数**
    /// 精确匹配键名，早先我们把缓冲区大小（4）当长度传，于是任何键都查不到。
    /// 这条断言让那个缺陷有牙。
    func testReadingBackConfiguredValue() throws {
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "font-size = 17.5\n".write(toFile: conf, atomically: true,
                                      encoding: .utf8)
        let app = try loadConfig(conf)
        XCTAssertEqual(app.float("font-size"), 17.5,
                       "配置里的字号应能按 Ghostty 键名读回（键长度必须给对）")
        XCTAssertEqual(app.double("background-opacity"), 1.0,
                       "未写的键应回落到引擎默认值（不透明）；"
                       + "该键是 f64，读它必须用 double 入口")
        XCTAssertNil(app.float("no-such-key"),
                     "不存在的键应返回 nil（不假装取到 0）")
        XCTAssertTrue(app.configDiagnostics.isEmpty, "干净配置不该有诊断")
    }

    // MARK: - 主题（配色集随包提供）

    /// 主题列表来自 `Resources/ghostty/themes`。开发态直接跑测试可执行文件
    /// 时那里是空的——所以这条断言**分两档**：没有资源目录时只要求
    /// 「显式为空、不假装有」；有资源目录（打包后的 .app / 显式注入）时
    /// 要求真能列出并**真被引擎接受**。
    func testThemeCatalogAndResolution() throws {
        let store = SettingsStore(terminalConfigPath: "/nonexistent/ghostty.conf")
        if let res = store.resourcesPath {
            let themes = store.availableThemes()
            XCTAssertFalse(themes.isEmpty,
                           "资源目录在（\(res)）却没有主题——检查打包是否拷了 themes")
            XCTAssertTrue(themes.contains("iTerm2 Default"),
                          "配色集应含上游带的那套（得到 \(themes.prefix(3))）")
        } else {
            XCTAssertTrue(store.availableThemes().isEmpty,
                          "没有资源目录时必须显式为空，不能假装有主题")
        }
    }

    /// 主题列表（纯 Swift 面）：注入一个资源目录，必须能列出里面的配色。
    /// 这条不碰引擎，故不受 libghostty 全局状态缓存的影响。
    func testThemeCatalogListing() throws {
        let dir = try tempDir()
        let themes = dir + "/ghostty/themes"
        try FileManager.default.createDirectory(atPath: themes,
                                                withIntermediateDirectories: true)
        for name in ["B Theme", "A Theme", ".hidden"] {
            try "background = #000000\n".write(
                toFile: themes + "/" + name, atomically: true, encoding: .utf8)
        }
        let store = SettingsStore(terminalConfigPath: dir + "/ghostty.conf",
                                  resourcesPath: dir + "/ghostty")
        XCTAssertEqual(store.availableThemes(), ["A Theme", "B Theme"],
                       "应列出配色并按名字排序、跳过点文件")

        let none = SettingsStore(terminalConfigPath: dir + "/ghostty.conf",
                                 resourcesPath: nil)
        XCTAssertTrue(none.availableThemes().isEmpty,
                      "没有资源目录时必须显式为空（UI 据此说明「未随包提供」）")
    }

    /// 主题**真的被引擎消费**：用**绝对路径**的主题文件（不经资源目录——
    /// 资源目录在进程内是首次初始化时缓存下来的，测试之间会互相影响，
    /// 而绝对路径这条路同样走 libghostty 的 theme 解析）。名字写错必须
    /// 显式失败，不静默用默认配色。
    func testThemeFileIsConsumedByEngine() throws {
        let dir = try tempDir()
        let themeFile = dir + "/My Theme"
        try """
        background = #123456
        foreground = #fedcba
        """.write(toFile: themeFile, atomically: true, encoding: .utf8)
        let conf = dir + "/ghostty.conf"
        try "theme = \"\(themeFile)\"\n".write(toFile: conf, atomically: true,
                                                encoding: .utf8)
        let app = try TerminalApp(configPath: conf)
        XCTAssertTrue(app.configDiagnostics.isEmpty,
                      "绝对路径主题应被引擎认（诊断：\(app.configDiagnostics)）")

        let bad = dir + "/bad.conf"
        try "theme = \"/no/such/theme/file\"\n".write(toFile: bad, atomically: true,
                                                    encoding: .utf8)
        XCTAssertThrowsError(try loadConfig(bad),
                             "找不到的主题应显式失败，不静默用默认配色")
    }

    /// 随包带的名字（`iTerm2 Default`）只有在**资源目录真的指对了**时才认。
    /// 资源目录由 C 侧在首次 init 时确定并缓存，测试进程里往往是开发态
    /// （没有 bundle）——所以这条只做**可诊断的两档断言**：有资源目录就
    /// 必须认；没有则跳过并说明。真·bundle 内的一致性由出包脚本的
    /// `--config-check` 断言（跑在装配好的 .app 里）。
    func testBundledThemeNameResolvesWhenResourcesPresent() throws {
        let store = SettingsStore(terminalConfigPath: "/nonexistent/ghostty.conf")
        guard store.resourcesPath != nil else {
            throw XCTSkip("开发态无资源目录（.app 内由出包脚本的 --config-check 覆盖）")
        }
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "theme = \"iTerm2 Default\"\n".write(toFile: conf, atomically: true,
                                                encoding: .utf8)
        let app = try loadConfig(conf)
        XCTAssertTrue(app.configDiagnostics.isEmpty,
                      "资源目录在（\(store.resourcesPath!)）却没认到随包主题")
    }

    /// 用共享 app **按配置路径重载**，语义与「用这份配置新建 app」一致
    /// （都能验配置是否被引擎接受），但不重建 app——重建会走出
    /// TerminalApp 文档里那个 mach 通知崩溃。
    private func loadConfig(_ path: String) throws -> TerminalApp {
        let app = try TerminalApp.shared()
        if let err = app.reloadConfig(path: path) {
            throw SettingsStoreError.invalid(err)
        }
        return app
    }

    /// `TerminalApp.shared()` 必须是**同一个**实例：libghostty 的全局状态
    /// 不随 app 释放而重置（`ghostty_init` 只跑一次），所以「建 app → 释放
    /// → 再建」会拿到一个没有全局状态的 app。这条断言守的就是它。
    func testSharedAppIsASingleton() throws {
        let a = try TerminalApp.shared()
        let b = try TerminalApp.shared()
        XCTAssertTrue(a === b, "shared() 必须返回同一实例（进程内单例）")

        // 配置重载落在**同一个** app 上：换了配置，共享实例上读到的值就变。
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "font-size = 11\n".write(toFile: conf, atomically: true,
                                    encoding: .utf8)
        XCTAssertNil(a.reloadConfig(path: conf))
        XCTAssertEqual(try TerminalApp.shared().float("font-size"), 11,
                       "重载后共享实例上应读到新值")
        try "font-size = 19\n".write(toFile: conf, atomically: true,
                                    encoding: .utf8)
        XCTAssertNil(b.reloadConfig(path: conf))
        XCTAssertEqual(try TerminalApp.shared().float("font-size"), 19,
                       "同一实例再重载应跟着变")
    }

    /// AppModel 启动时要把**用户配置**重载进共享 app（不能让 shared 的
    /// configPath 参数被静默忽略——单例已存在时它确实会被忽略）。
    @MainActor
    func testAppModelAppliesUserConfigToSharedApp() throws {
        let dir = try tempDir()
        let conf = dir + "/ghostty.conf"
        try "font-size = 27\n".write(toFile: conf, atomically: true,
                                    encoding: .utf8)
        let cfg = dir + "/ssh_config"
        try "Host web\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known",
                             settings: SettingsStore(terminalConfigPath: conf))
        XCTAssertNil(model.terminalConfigError, "合法配置不该报错")
        XCTAssertEqual(model.terminalApp?.float("font-size"), 27,
                       "AppModel 应把用户配置重载进共享 app（否则字体设置不生效）")
    }

    private func grid(of configPath: String) throws -> (UInt16, UInt16) {
        let app = try loadConfig(configPath)
        let surface = try TerminalSurface(
            app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        return (surface.gridSize.cols, surface.gridSize.rows)
    }

    // MARK: - SettingsStore 写侧语义（不依赖渲染）

    func testMergingIsLineFaithful() {
        let original = """
        # 我的笔记
        font-size = 12

        theme = dark
        cursor-style = block
        """
        let out = SettingsStore.merging(original,
                                        edits: [("font-size", "16"),
                                                ("background-opacity", "0.9")])
        XCTAssertTrue(out.contains("# 我的笔记"), "注释必须原样保留")
        XCTAssertTrue(out.contains("font-size = 16"), "目标键应被替换")
        XCTAssertFalse(out.contains("font-size = 12"), "旧值不应残留")
        XCTAssertTrue(out.contains("theme = dark"), "其他键不动")
        XCTAssertTrue(out.contains("cursor-style = block"), "顺序也不动")
        // 字面量末尾没有换行（Swift 的 """ 会吃掉最后一个换行），所以
        // 结果同样不该凭空多一个——这里同时验「追加」与「换行结构保真」。
        XCTAssertTrue(out.hasSuffix("background-opacity = 0.9"),
                      "新键追加在末尾（得到 \(out.debugDescription)）")
        XCTAssertFalse(out.hasSuffix("\n"), "原文无尾换行，结果也不该有")
        XCTAssertTrue(out.contains("font-size = 16\n\ntheme = dark"),
                      "中途的空行是用户排版，必须留着")
    }

    /// 删除语义：值传 nil → 那一行被摘掉，其余行逐字不变。
    func testMergingDeletesWithNilValue() {
        let original = "font-size = 12\ntheme = dark\n"
        let out = SettingsStore.merging(original, edits: [("font-size", nil)])
        XCTAssertEqual(out, "theme = dark\n", "删掉目标行，其余逐字保留")
    }

    /// 注释掉的同名键是**用户的笔记**，不能被当成待改的键。
    func testMergingLeavesCommentedKeysAlone() {
        let original = "# font-size = 99\n"
        let out = SettingsStore.merging(original, edits: [("font-size", "14")])
        XCTAssertTrue(out.contains("# font-size = 99"), "注释行不动")
        XCTAssertTrue(out.contains("font-size = 14"), "新行追加")
    }

    func testValueReadsLastOccurrence() {
        let text = "font-size = 12\nfont-size = 18\n"
        XCTAssertEqual(SettingsStore.value(of: "font-size", in: text), "18",
                       "与 libghostty 的覆盖语义一致：最后一次胜出")
    }

    func testSaveBacksUpAndDetectsExternalEdit() throws {
        let dir = try tempDir()
        let path = dir + "/ghostty.conf"
        try "font-size = 12\n".write(toFile: path, atomically: true, encoding: .utf8)

        let store = SettingsStore(terminalConfigPath: path)
        try store.saveTerminalConfig("font-size = 16\n")
        XCTAssertTrue(FileManager.default.fileExists(atPath: path + ".bak"),
                      "写回前应留一份备份")
        XCTAssertEqual(SettingsStore.value(of: "font-size", in: store.rawConfig()),
                       "16", "新值应已落盘")
        XCTAssertFalse(store.isExternallyModified(), "自己写的不算外部修改")

        // 外部改过 → 拒绝写（不静默覆盖别人的编辑）。
        try "font-size = 20\n".write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertTrue(store.isExternallyModified())
        XCTAssertThrowsError(try store.saveTerminalConfig("font-size = 30\n")) { e in
            XCTAssertTrue("\(e)".contains("外部修改"), "应报冲突（得到 \(e)）")
        }
        XCTAssertEqual(SettingsStore.value(of: "font-size",
                                          in: try String(contentsOfFile: path,
                                                         encoding: .utf8)),
                       "20", "拒绝写之后外部内容必须还在")
    }

    func testAppPreferencesRoundTrip() throws {
        // 用独立的 defaults suite，别污染用户真实的偏好。
        let suite = "dev.ghosterm.tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }

        var prefs = AppPreferences(defaults: defaults)
        XCTAssertTrue(prefs.shellOnLaunch, "默认应在启动时新建会话")
        XCTAssertFalse(prefs.lockTabsOnLaunch, "默认不锁标签页")
        prefs.shellOnLaunch = false
        prefs.defaultShell = "/bin/bash"
        XCTAssertFalse(prefs.shellOnLaunch)
        XCTAssertEqual(prefs.resolvedShell, "/bin/bash")

        prefs.defaultShell = ""
        XCTAssertEqual(prefs.resolvedShell,
                       ProcessInfo.processInfo.environment["SHELL"],
                       "留空应回落到 $SHELL")
    }
}
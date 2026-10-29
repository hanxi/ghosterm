import AppKit
@testable import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class ChromePaletteTests: XCTestCase {
    private func image(_ f: TabWindowFixture) throws -> NSBitmapImageRep {
        f.settle()
        let image = try XCTUnwrap(f.root.bitmapImageRepForCachingDisplay(in: f.root.bounds))
        f.window.effectiveAppearance.performAsCurrentDrawingAppearance {
            f.root.cacheDisplay(in: f.root.bounds, to: image)
        }
        return image
    }
    private func assertColor(_ image: NSBitmapImageRep, _ f: TabWindowFixture,
                             x: CGFloat, y: CGFloat, rgb: [CGFloat], _ message: String,
                             file: StaticString = #filePath, line: UInt = #line) throws {
        let scale = CGFloat(image.pixelsWide) / f.root.bounds.width
        var pixel = [UInt](repeating: 0, count: image.samplesPerPixel)
        image.getPixel(&pixel, atX: Int(x * scale), y: Int(y * scale))
        // cacheDisplay 使用显示器 ICC；先把固定 sRGB 期望值转到位图色域再比较原始分量。
        let expected = try XCTUnwrap(NSColor(srgbRed: rgb[0] / 255, green: rgb[1] / 255,
                                             blue: rgb[2] / 255, alpha: 1).usingColorSpace(image.colorSpace))
        for (actual, value) in zip(pixel.prefix(3), [expected.redComponent, expected.greenComponent, expected.blueComponent]) {
            XCTAssertEqual(CGFloat(actual) / 255, value, accuracy: 0.015, message, file: file, line: line)
        }
    }
    private func restoreGap() {
        let old = UserDefaults.standard.object(forKey: PaneGap.key)
        UserDefaults.standard.removeObject(forKey: PaneGap.key)
        addTeardownBlock { UserDefaults.standard.set(old, forKey: PaneGap.key) }
    }

    func testNavigationTitleAndTerminalShareThemeAfterReload() throws {
        restoreGap()
        let f = try TabWindowFixture(); defer { f.close() }
        let (tab, surface) = try f.shell("palette")
        tab.split(axis: .horizontal, with: TerminalPane())
        tab.activePaneID = tab.panes[0].id
        let original = surface.view
        for (bg, fg, rgb) in [("252a33", "d1d8e2", [37, 42, 51] as [CGFloat]),
                              ("f6f7f9", "35404e", [246, 247, 249])] {
            // 刻意使用与终端相反的系统外观，不能依赖 .primary 恰巧可读。
            f.window.appearance = NSAppearance(named: bg == "252a33" ? .aqua : .darkAqua)
            try f.model.replaceTerminalConfig("background = #\(bg)\nforeground = #\(fg)\n")
            for placement in TabPlacement.allCases {
                f.model.preferences.tabPlacement = placement
                f.settle(0.2); surface.setVisible(true); surface.draw(); f.settle(0.3)
                try f.capture("palette-" + placement.rawValue + "-" + bg)
                let terminalHost = try f.terminalHost(for: tab.panes[0])
                let terminalRect = terminalHost.convert(terminalHost.bounds, to: f.root)
                let shot = try image(f)
                let offset: CGFloat = placement == .sidebar ? DS.sidebarWidth + 1 : 0
                let top: CGFloat = placement == .top ? WindowChrome.barHeight + 1 : 0
                XCTAssertEqual(terminalRect.minX, offset, accuracy: 0.5, "默认窗格必须贴边，不能有外留白")
                try assertColor(shot, f, x: placement == .sidebar ? 160 : 800, y: 15, rgb: rgb,
                                "导航必须使用终端背景色")
                if placement == .sidebar {
                    try assertColor(shot, f, x: 100, y: 500, rgb: rgb, "侧栏列表不能叠加系统材质")
                }
                try assertColor(shot, f, x: offset + 400, y: top + 3, rgb: rgb, "窗格标题必须融入终端")
                try assertColor(shot, f, x: offset + 400, y: top + 25, rgb: rgb, "窗格标题不能有底部描边")
                try assertColor(shot, f, x: offset + 100, y: 450, rgb: rgb, "真实终端背景必须与导航一致")
                try assertColor(shot, f, x: offset, y: 200, rgb: rgb, "贴边窗格不能叠加整圈描边")
                let plus = try XCTUnwrap(f.views(NSButton.self).first { $0.identifier?.rawValue == "new-connection" })
                let ink = try XCTUnwrap(plus.contentTintColor?.usingColorSpace(.deviceRGB))
                XCTAssertEqual(ink.redComponent, bg == "252a33" ? 209.0 / 255 : 53.0 / 255,
                               accuracy: 0.01, "原生加号必须跟随终端前景色")
                XCTAssertTrue(tab.activePane?.surface?.view === original, "配色和标签位置切换不能重建终端")
            }
        }
    }

    func testGapOnlyChangesSpacingAndDividerInk() throws {
        restoreGap()
        let f = try TabWindowFixture(); defer { f.close() }
        try f.model.replaceTerminalConfig("background = #303030\nforeground = #dddddd\n")
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .horizontal, with: TerminalPane()); f.model.tabs.add(tab)
        f.model.preferences.tabPlacement = .top
        for gap in [0, 10, 0] {
            UserDefaults.standard.set(gap, forKey: PaneGap.key)
            f.settle()
            let shot = try image(f)
            // 拖动命中区宽 6pt，不能整条涂色或在抓帧时变成不透明黑条。
            for x in [CGFloat(498), 502] {
                try assertColor(shot, f, x: x, y: 200, rgb: [48, 48, 48], "分隔条命中区不能涂满底色")
            }
            if gap > 0 {
                try assertColor(shot, f, x: 500, y: 200, rgb: [48, 48, 48], "留白时分隔区域必须透明")
            } else {
                let scale = CGFloat(shot.pixelsWide) / f.root.bounds.width
                var center = [UInt](repeating: 0, count: shot.samplesPerPixel)
                var beside = center
                shot.getPixel(&center, atX: Int(500 * scale), y: Int(200 * scale))
                shot.getPixel(&beside, atX: Int(498 * scale), y: Int(200 * scale))
                XCTAssertGreaterThan(Int(center[0]), Int(beside[0]) + 3, "贴边分屏必须保留细分隔线")
            }
        }
    }

    /// 真生产窗格：上下终端 + 右侧文件，切焦点和标题开关均不能丢顶边。
    func testMixedPaneEdgesFollowFocusAndGap() throws {
        restoreGap()
        let defaults = UserDefaults.standard
        let oldBar = defaults.object(forKey: PaneTitleBar.visibleKey)
        defer { defaults.set(oldBar, forKey: PaneTitleBar.visibleKey) }
        let f = try TabWindowFixture(); defer { f.close() }
        try f.model.replaceTerminalConfig("background = #303030\nforeground = #dddddd\n")
        f.model.preferences.tabPlacement = .sidebar
        f.window.setContentSize(NSSize(width: 1080, height: 675))
        let first = TerminalPane(), second = TerminalPane()
        for pane in [first, second] { pane.attachForTesting(surface: try TerminalSurface(app: TerminalApp.shared())) }
        let tab = TerminalTab(kind: .localShell, pane: first)
        tab.split(axis: .vertical, with: second)
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: f.directory.path))
        let files = WorkspaceFilePane(remote: false, panel: panel)
        tab.addFilePane(files); panel.start()
        tab.workspaceLayout = .group([.group([.leaf(first.id), .leaf(second.id)], axis: .vertical),
                                      .leaf(files.id)], axis: .horizontal)
        f.model.tabs.add(tab)
        for gap in [10, 0] {
            defaults.set(gap, forKey: PaneGap.key)
            for bar in [true, false] {
                defaults.set(bar, forKey: PaneTitleBar.visibleKey)
                for focused in [second.id, files.id] {
                    tab.activeWorkspacePaneID = focused
                    let shot = try image(f)
                    let workspace = try XCTUnwrap(f.views(WorkspaceHostView.self).first)
                    let scale = CGFloat(shot.pixelsWide) / f.root.bounds.width
                    func pixel(_ x: CGFloat, _ y: CGFloat) -> [Int] {
                        var p = [UInt](repeating: 0, count: shot.samplesPerPixel)
                        shot.getPixel(&p, atX: Int(x * scale), y: Int(y * scale))
                        return [Int(p[0]), Int(p[1]), Int(p[2])]
                    }
                    func chroma(_ p: [Int]) -> Int { p.max()! - p.min()! }
                    for id in [first.id, second.id, files.id] {
                        let host = try XCTUnwrap(workspace.hosts[id])
                        let raw = host.convert(host.bounds, to: f.root)
                        let top = f.root.isFlipped ? raw.minY : f.root.bounds.height - raw.maxY
                        let rect = CGRect(x: raw.minX + CGFloat(gap) / 2, y: top + CGFloat(gap) / 2,
                                          width: raw.width - CGFloat(gap), height: raw.height - CGFloat(gap))
                        // 取 1/4、1/2、3/4 三处，不能拿标题前的一小段线冒充贯穿顶边。
                        for fraction in [CGFloat(0.25), 0.5, 0.75] {
                            let c = chroma(pixel(rect.minX + rect.width * fraction, rect.minY + 0.5))
                            if id == focused {
                                XCTAssertGreaterThan(c, 40, "混排当前窗格必须有贯穿顶边的蓝线（含隐藏标题）")
                            } else {
                                XCTAssertLessThan(c, 10, "混排非当前窗格不能显示蓝线")
                            }
                        }
                        if gap > 0 {
                            let edge = pixel(rect.minX + 0.25, rect.minY + 18)
                            let inside = pixel(rect.minX + 3, rect.minY + 18)
                            if id == focused {
                                XCTAssertGreaterThan(chroma(edge), 20, "留白当前窗格必须保留淡蓝外框")
                            } else {
                                XCTAssertGreaterThan(zip(edge, inside).map { abs($0 - $1) }.max()!, 2,
                                                     "留白非当前窗格必须保留中性细框")
                            }
                        }
                    }
                }
            }
        }
    }

    func testSidebarNewTabLabelRemainsReadableAfterThemeSwitch() throws {
        restoreGap()
        let f = try TabWindowFixture(); defer { f.close() }
        f.model.preferences.tabPlacement = .sidebar
        f.model.tabs.add(TerminalTab(kind: .localShell))
        for dark in [false, true, false] {
            f.window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            try f.model.replaceTerminalConfig(dark
                ? "background = #252a33\nforeground = #d1d8e2\n"
                : "background = #f6f7f9\nforeground = #35404e\n")
            let shot = try image(f)
            let button = try XCTUnwrap(f.views(NSButton.self).first { $0.identifier?.rawValue == "new-connection" })
            let cell = try XCTUnwrap(button.cell as? NSButtonCell)
            let rect = button.convert(cell.titleRect(forBounds: button.bounds), to: f.root)
            let top = f.root.isFlipped ? rect.minY : f.root.bounds.height - rect.maxY
            let scale = CGFloat(shot.pixelsWide) / f.root.bounds.width
            var ink = 0
            for y in Int(top * scale)..<Int((top + rect.height) * scale) {
                for x in Int(rect.minX * scale)..<Int(rect.maxX * scale) {
                    var pixel = [UInt](repeating: 0, count: shot.samplesPerPixel)
                    shot.getPixel(&pixel, atX: x, y: y)
                    let luma = (Int(pixel[0]) * 299 + Int(pixel[1]) * 587 + Int(pixel[2]) * 114) / 1000
                    if dark ? luma > 140 : luma < 140 { ink += 1 }
                }
            }
            XCTAssertGreaterThan(ink, 30, "侧栏新建标签页文字必须随深浅主题保持可读")
            try f.capture("new-label-" + (dark ? "dark" : "light"))
        }
    }

    func testConditionalThemeUsesResolvedColors() throws {
        restoreGap()
        let f = try TabWindowFixture(); defer { f.close() }
        let old = f.model.appearance
        defer { f.model.appearance = old }
        let light = f.directory.appendingPathComponent("light-theme"), dark = f.directory.appendingPathComponent("dark-theme")
        try "background = #f6f7f9\nforeground = #35404e\n".write(to: light, atomically: true, encoding: .utf8)
        try "background = #252a33\nforeground = #d1d8e2\n".write(to: dark, atomically: true, encoding: .utf8)
        try f.model.replaceTerminalConfig("theme = light:\"\(light.path)\",dark:\"\(dark.path)\"\n")
        f.model.tabs.add(TerminalTab(kind: .localShell))
        for (appearance, rgb) in [(AppearancePreference.dark, [37, 42, 51] as [CGFloat]),
                                  (.light, [246, 247, 249]), (.dark, [37, 42, 51])] {
            f.model.appearance = appearance
            f.model.applyColorSchemeToTerminal()
            let shot = try image(f)
            try assertColor(shot, f, x: 160, y: 15, rgb: rgb, "条件主题切换必须更新导航底色")
        }
    }
}

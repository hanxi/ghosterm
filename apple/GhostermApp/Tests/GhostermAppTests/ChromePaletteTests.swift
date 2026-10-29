import AppKit
import GhostermAppModel
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
                try assertColor(shot, f, x: offset, y: 200, rgb: rgb, "窗格不能绘制整圈焦点描边")
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

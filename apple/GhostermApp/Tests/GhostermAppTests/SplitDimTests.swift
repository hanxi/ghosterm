// SplitDimTests — 非聚焦窗格变暗的**纯规则**断言 + **接线**断言。
//
// 规则（1 - unfocused-split-opacity 当 alpha、unfocused-split-fill 回落
// background）照抄上游 ghostty（Config.zig 默认 0.7；SurfaceView.swift 拿
// 背景色当 fill 盖一层）。规则错了会「变暗了但颜色不对 / 该不变暗却在变暗」，
// 屏幕上不报错——故矩阵穷举 + 一条端到端读数断言。
import AppKit
import GhostermAppModel
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp

final class SplitDimTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private let bg = SplitDim.RGB(r: 40, g: 44, b: 52)
    private let fill = SplitDim.RGB(r: 10, g: 20, b: 30)

    /// 把**共享**的 TerminalApp 配置恢复成默认（本套用例改了背景/遮罩键，
    /// 而同进程别的用例——Pixel/Status 渲染——还要默认背景）。
    private func resetEngineConfig(in dir: String) {
        let reset = dir + "/reset.conf"
        try? "".write(toFile: reset, atomically: true, encoding: .utf8)
        _ = try? TerminalApp.shared().reloadConfig(path: reset)
    }

    // MARK: - 纯规则（穷举）

    /// 上游默认：opacity 0.7、无 fill → 用背景色，alpha = 0.3。
    func testDefaultOpacityDimsTowardBackground() {
        let d = SplitDim.resolve(opacity: nil, fill: nil, background: bg)
        XCTAssertEqual(d?.alpha ?? -1, 0.3, accuracy: 1e-9,
                       "默认保留 0.7 → 遮罩 alpha 应为 0.3")
        XCTAssertEqual(d?.red ?? -1, 40.0 / 255, accuracy: 1e-9,
                       "没设 fill 时遮罩颜色应是**背景色**（不是黑色）")
        XCTAssertEqual(d?.green ?? -1, 44.0 / 255, accuracy: 1e-9)
        XCTAssertEqual(d?.blue ?? -1, 52.0 / 255, accuracy: 1e-9)
    }

    /// 显式 fill 优先于 background。
    func testExplicitFillWinsOverBackground() {
        let d = SplitDim.resolve(opacity: 0.5, fill: fill, background: bg)
        XCTAssertEqual(d?.alpha ?? -1, 0.5, accuracy: 1e-9)
        XCTAssertEqual(d?.red ?? -1, 10.0 / 255, accuracy: 1e-9,
                       "设了 unfocused-split-fill 就该用它")
    }

    /// opacity ≥ 1 = 用户关掉了这个特性 → 不变暗（返回 nil）。
    func testOpacityOneDisablesDimming() {
        XCTAssertNil(SplitDim.resolve(opacity: 1.0, fill: fill, background: bg),
                     "opacity=1 是上游的「关掉」口径，必须不变暗")
        XCTAssertNil(SplitDim.resolve(opacity: 1.5, fill: fill, background: bg),
                     "大于 1 同样当关掉")
    }

    /// 引擎把 opacity 夹进 [0.15, 1]（上游 Config.zig 同口径）——0.1 应被抬到
    /// 0.15，于是 alpha = 0.85，而不是「几乎全黑」。
    func testOpacityIsClampedToLowerBound() {
        let d = SplitDim.resolve(opacity: 0.1, fill: nil, background: bg)
        XCTAssertEqual(d?.alpha ?? -1, 0.85, accuracy: 1e-9,
                       "0.1 应被夹到 0.15 → alpha 0.85")
    }

    /// 颜色一个都读不到 → 不猜，返回 nil（与「显式关掉」不同路径，同样不动）。
    func testNoColorMeansNoDim() {
        XCTAssertNil(SplitDim.resolve(opacity: 0.7, fill: nil, background: nil),
                     "连背景色都读不到时不该拿黑色假装")
    }

    // MARK: - 接线（端到端读引擎）

    /// app 里的 splitDim 真的从**引擎的生效配置**读出来：换主题（背景变）→
    /// 遮罩颜色跟着变。这条串起 `gt_ghostty_config_get_color` → `SplitDim`。
    @MainActor
    func testAppModelReadsDimFromEngineConfig() throws {
        let dir = NSTemporaryDirectory() + "ghosterm-dim-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let conf = dir + "/ghostty.conf"
        addTeardownBlock { [dir] in self.resetEngineConfig(in: dir) }

        try "background = #0a0b0c\nunfocused-split-opacity = 0.5\n"
            .write(toFile: conf, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: dir + "/ssh_config",
                             knownHostsPath: dir + "/known_hosts",
                             settings: SettingsStore(terminalConfigPath: conf))
        let dim = try XCTUnwrap(model.splitDim, "应读出遮罩参数")
        XCTAssertEqual(dim.alpha, 0.5, accuracy: 1e-9)
        XCTAssertEqual(dim.red, 10.0 / 255, accuracy: 1e-9, "应取到配置里的背景色")
        XCTAssertEqual(dim.green, 11.0 / 255, accuracy: 1e-9)
        XCTAssertEqual(dim.blue, 12.0 / 255, accuracy: 1e-9)

        // 关掉变暗 → splitDim 变 nil（接线真的跟着配置走）。
        try "background = #0a0b0c\nunfocused-split-opacity = 1\n"
            .write(toFile: conf, atomically: true, encoding: .utf8)
        XCTAssertNil(model.terminalApp?.reloadConfig(path: conf))
        XCTAssertNil(model.splitDim, "opacity=1 之后不该再变暗")
    }

    // MARK: - 接线（端到端抓像素）

    /// 抓一帧的全部 RGB 像素。
    private struct Frame {
        let width: Int
        let height: Int
        let pixels: [[(Int, Int, Int)]]

        /// 指定矩形里「偏红」的像素数（r 明显高于 g/b）。
        ///
        /// 判据刻意不要求「纯红」：遮罩是半透明的，白底/黑底叠上去都会偏灰红，
        /// 用阈值「r > g 且 r > b 各 20」两种底色都认得出来。
        func reddish(x0: Int, x1: Int, y0: Int, y1: Int) -> Int {
            var n = 0
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    let p = pixels[y][x]
                    if p.0 > p.1 + 20 && p.0 > p.2 + 20 { n += 1 }
                }
            }
            return n
        }
    }

    @MainActor
    private func renderWindow(model: AppModel) -> (Frame, Double) {
        let size = NSSize(width: 900, height: 600)
        let host = makeRootHostingView(model: model,
                                       initialShowsSidebar: false)
        host.frame = NSRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        window.isOpaque = true
        window.backgroundColor = .windowBackgroundColor
        host.wantsLayer = true
        // 动态 `NSColor` 的 `.cgColor` 按**线程当前外观**解（机理见
        // `SettingsBackgroundView`），而那个线程态在别的用例渲染完深色后
        // 不会自己还原——裸取会解出暗底色，整帧发暗，按亮度数控件像素的
        // 断言就全红。显式在本窗口的外观下解。
        window.effectiveAppearance.performAsCurrentDrawingAppearance {
            host.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        }
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }

        for _ in 0..<12 {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
            XCTFail("拿不到位图缓存")
            return (Frame(width: 0, height: 0, pixels: []), 1)
        }
        host.cacheDisplay(in: host.bounds, to: rep)
        var pixels: [[(Int, Int, Int)]] = []
        for y in 0..<rep.pixelsHigh {
            var row: [(Int, Int, Int)] = []
            for x in 0..<rep.pixelsWide {
                let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB)
                row.append((Int((c?.redComponent ?? 0) * 255),
                            Int((c?.greenComponent ?? 0) * 255),
                            Int((c?.blueComponent ?? 0) * 255)))
            }
            pixels.append(row)
        }
        let scale = Double(rep.pixelsWide) / Double(size.width)
        return (Frame(width: rep.pixelsWide, height: rep.pixelsHigh,
                      pixels: pixels), scale)
    }

    /// 端到端：非聚焦窗格真的被**画**上了遮罩色，聚焦窗格没有。
    ///
    /// 造两个都没有 terminal 面的窗格（都画 `textBackgroundColor`），于是两半
    /// 的底色一模一样——差别只能来自遮罩。fill 用纯红是为了让判据（r 高于 g/b）
    /// 与系统深浅色无关。
    ///
    /// 配一条对照：把 `unfocused-split-opacity` 设成 1（上游口径 = 关掉变暗）
    /// 后，两半都不该再偏红。只删遮罩会让第一条假绿，但对照会红——出手就有指向。
    @MainActor
    func testUnfocusedPaneIsDimmedWithFillColor() throws {
        let dir = NSTemporaryDirectory() + "ghosterm-dimpx-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let conf = dir + "/ghostty.conf"
        try "Host web\n  HostName h\n".write(toFile: dir + "/ssh_config",
                                              atomically: true, encoding: .utf8)
        func writeConfig(opacity: Double) throws {
            try "unfocused-split-fill = #ff0000\nunfocused-split-opacity = \(opacity)\n"
                .write(toFile: conf, atomically: true, encoding: .utf8)
        }
        func makeModel() -> AppModel {
            isolatedAppModel(configPath: dir + "/ssh_config",
                     knownHostsPath: dir + "/known_hosts",
                     settings: SettingsStore(terminalConfigPath: conf))
        }
        // 恢复引擎配置（共享单例，别的用例还要用默认背景）。
        addTeardownBlock { [dir] in self.resetEngineConfig(in: dir) }

        try writeConfig(opacity: 0.7)
        let onModel = makeModel()
        // 两个窗格都还没有 terminal 面（连上之前就是这块底色），分屏后
        // **新窗格**是当前窗格 → 左边那个是非聚焦的，应该被染红。
        let onTab = TerminalTab(kind: .localShell, pane: TerminalPane())
        onTab.split(axis: .horizontal, with: TerminalPane())
        onModel.tabs.add(onTab)
        let (on, scale) = renderWindow(model: onModel)

        let widthOn = on.width, heightOn = on.height
        let paneTop = Int((WindowChrome.barHeight + 4) * scale)
        let paneBottom = heightOn - Int(4 * scale)
        let leftOn = on.reddish(x0: Int(6 * scale), x1: Int((900.0 / 2 - 6) * scale),
                                y0: paneTop, y1: paneBottom)
        let rightOn = on.reddish(x0: Int((900.0 / 2 + 6) * scale),
                                 x1: widthOn - Int(6 * scale),
                                 y0: paneTop, y1: paneBottom)

        XCTAssertGreaterThan(leftOn, 5000,
            "非聚焦（左）窗格应盖上一层红色遮罩（偏红像素 \(leftOn)）——"
            + "遮罩没画出来这条就红")
        XCTAssertGreaterThan(leftOn, rightOn * 4 + 5000,
            "聚焦（右）窗格不该被遮罩染色：左 \(leftOn) vs 右 \(rightOn)")

        // 对照：关掉变暗（opacity=1）→ 两半都不偏红。
        try writeConfig(opacity: 1)
        XCTAssertNil(onModel.terminalApp?.reloadConfig(path: conf))
        let (off, _) = renderWindow(model: onModel)
        let leftOff = off.reddish(x0: Int(6 * scale), x1: Int((900.0 / 2 - 6) * scale),
                                  y0: paneTop, y1: paneBottom)
        XCTAssertLessThan(leftOff, leftOn / 4,
            "opacity=1 之后不该再染色（对照：\(leftOff) vs 开着的 \(leftOn)）")
    }

}

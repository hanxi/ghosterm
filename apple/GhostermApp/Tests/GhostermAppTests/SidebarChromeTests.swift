// SidebarChromeTests — 「侧栏 / 顶栏」的几何与高亮**像素级**断言。
//
// 为什么必须抓像素：这几条都是「不报错、只在屏幕上不对」的改动——
//   · 侧栏切换按钮压在红绿灯上（用户截图里那团蓝色高亮）；
//   · 侧栏里当前标签页看不出来是高亮的那行。
// 抓帧用**进程内**的 `cacheDisplay`（不需要屏幕录制权限，与 PixelRenderTests
// 同一套办法）。窗口自己的红绿灯不在抓到的位图里——正好：我们断言的就是
// 「我们的内容有没有把控件画到红绿灯的地盘上」（那团高亮就是这么来的）。
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class SidebarChromeTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 抓帧

    private struct Frame {
        let width: Int
        let height: Int
        let pixels: [[(Int, Int, Int)]]

        func pixel(x: Int, y: Int) -> (Int, Int, Int) {
            pixels[min(max(y, 0), height - 1)][min(max(x, 0), width - 1)]
        }

        /// 指定矩形里「像控件」的像素数：明显暗（字形）或带颜色（控件底）。
        func controlPixels(x0: Int, x1: Int, y0: Int, y1: Int) -> Int {
            var n = 0
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    let p = pixel(x: x, y: y)
                    let luma = (p.0 * 299 + p.1 * 587 + p.2 * 114) / 1000
                    let mx = max(p.0, max(p.1, p.2)), mn = min(p.0, min(p.1, p.2))
                    if luma < 140 || (mx - mn) > 45 { n += 1 }
                }
            }
            return n
        }

        func meanLuma(x0: Int, x1: Int, y0: Int, y1: Int) -> Double {
            var sum = 0.0, n = 0.0
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    let p = pixel(x: x, y: y)
                    sum += Double((p.0 * 299 + p.1 * 587 + p.2 * 114) / 1000)
                    n += 1
                }
            }
            return n > 0 ? sum / n : 0
        }
    }

    /// 把 SwiftUI 视图树渲染成位图（进程内抓帧），返回帧与 Retina 比例。
    private func render<V: View>(_ view: V, size: NSSize) -> (Frame, Double) {
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        // 离屏抓帧要显式铺底：材质（顶栏 / 侧栏）在没有不透明底时抓到全黑，
        // 那样「控件像素」会把整片黑都算进去，断言就没意义了。
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
        return (Frame(width: rep.pixelsWide, height: rep.pixelsHigh, pixels: pixels),
                scale)
    }

    /// 一个装了标签页的模型（不连任何东西，只喂视图）。
    @MainActor
    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "ghosterm-chrome-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true,
                                            encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts",
                                     settings: SettingsStore(terminalConfigPath: dir + "/ghostty"))
        try model.replaceTerminalConfig("background = #f6f6f6\nforeground = #242424\n")
        return model
    }

    /// 整窗渲染（含侧栏 + 顶栏；终端面没挂 surface 时只画一块底色）。
    @MainActor
    private func renderWindow(sidebar: Bool) throws -> (Frame, Double) {
        let model = try makeModel()
        model.tabs.add(TerminalTab(kind: .localShell))
        model.preferences.tabPlacement = sidebar ? .sidebar : .top
        let view = RootView(model: model)
        return render(view, size: NSSize(width: 900, height: 600))
    }

    // 顶带只留系统红绿灯；侧栏新建入口移到标签列表末尾。
    @MainActor
    func testSidebarHeaderOnlyReservesTrafficLights() throws {
        let (frame, scale) = try renderWindow(sidebar: true)
        XCTAssertGreaterThan(frame.width, 0, "抓帧失败")
        let header = frame.controlPixels(x0: 0, x1: Int(DS.sidebarWidth * scale),
                                         y0: 0, y1: Int(WindowChrome.barHeight * scale))
        XCTAssertEqual(header, 0, "侧栏顶带只预留红绿灯，不再画新建按钮")
    }

    @MainActor
    func testTopBarShowsThePlusAtTheRightEdge() throws {
        let (frame, scale) = try renderWindow(sidebar: false)
        let underLights = frame.controlPixels(x0: 0, x1: Int(WindowChrome.trafficLightsWidth * scale),
                                               y0: 0, y1: Int(WindowChrome.barHeight * scale))
        XCTAssertEqual(underLights, 0, "顶部标签不能压在红绿灯上")
        let rightEdge = frame.controlPixels(x0: frame.width - Int(44 * scale), x1: frame.width,
                                           y0: 0, y1: Int(WindowChrome.barHeight * scale))
        XCTAssertGreaterThan(rightEdge, 10, "顶部加号应画在最右端")
    }

    /// 侧栏底部不再有那条 ⚙︎ footer。
    ///
    /// 删它的理由是重复（设置在 ⌘, / Ghosterm ▸ 设置…）；这条钉住它别长
    /// 回来，也钉住「删掉的东西真的从屏幕上消失了」——光删代码、忘了删
    /// `Divider` 之类的残留，视觉上还是多一条线。
    @MainActor
    func testSidebarHasNoBottomSettingsFooter() throws {
        let (frame, scale) = try renderWindow(sidebar: true)
        let footer = frame.controlPixels(
            x0: 0, x1: Int(DS.sidebarWidth * scale),
            y0: frame.height - Int(34 * scale), y1: frame.height)
        XCTAssertEqual(footer, 0,
                       "侧栏底部那条 ⚙︎ footer 已删，不该还有控件像素"
                       + "（实际 \(footer) 个）")
    }

    // MARK: - 3. 当前标签页高亮

    /// 侧栏里「当前标签页」那一行要看得出来：同一份侧栏渲染两次
    /// （当前行是 A / 没有当前行），**行区**的亮度必须有差；而**空白区**
    /// 必须一模一样——差异只能来自那一行，不是整幅画面在变。
    @MainActor
    func testActiveTabRowIsHighlighted() throws {
        let model = try makeModel()
        let tabA = TerminalTab(kind: .localShell)
        let tabB = TerminalTab(kind: .sftp(RemoteTarget(pattern: "web", user: "u", host: "h", port: 22)))
        model.tabs.add(tabA)
        model.tabs.add(tabB)

        let size = NSSize(width: DS.sidebarWidth, height: 420)
        func renderSidebar() -> (Frame, Double) {



            return render(Sidebar(model: model, tabs: model.tabs),
                          size: size)
        }

        model.tabs.activeTabID = tabA.id
        let (withActive, scale) = renderSidebar()
        model.tabs.activeTabID = nil
        let (withoutActive, _) = renderSidebar()

        // 两行所在的行区（顶带以下、第二行结束前）。
        let rowsY0 = Int(40 * scale), rowsY1 = Int(150 * scale)
        let rowsX0 = Int(4 * scale), rowsX1 = Int((DS.sidebarWidth - 4) * scale)
        let lit = withActive.meanLuma(x0: rowsX0, x1: rowsX1,
                                      y0: rowsY0, y1: rowsY1)
        let unlit = withoutActive.meanLuma(x0: rowsX0, x1: rowsX1,
                                           y0: rowsY0, y1: rowsY1)
        XCTAssertGreaterThan(abs(lit - unlit), 1.0,
                             "当前标签页那一行该有高亮块（行区亮度应明显不同）；"
                             + "得到 \(lit) vs \(unlit)")

        // 指向性：行区以外的空白区必须一致——差异确实来自那一行。
        let emptyY0 = Int(220 * scale), emptyY1 = Int(330 * scale)
        let e1 = withActive.meanLuma(x0: rowsX0, x1: rowsX1,
                                     y0: emptyY0, y1: emptyY1)
        let e2 = withoutActive.meanLuma(x0: rowsX0, x1: rowsX1,
                                        y0: emptyY0, y1: emptyY1)
        XCTAssertLessThan(abs(e1 - e2), 0.5,
                          "空白区不该跟着变（差异应只来自当前行）；"
                          + "得到 \(e1) vs \(e2)")
    }
}

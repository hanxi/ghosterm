// TerminalStatusRenderTests — OSC 7501 角标 / OSC 133「上一条命令」
// 这两处 UI「真的画到屏幕像素上」。
//
// 侧栏/顶栏那套（SidebarChromeTests）证明的是**布局**；这份补的是
// capability-matrix 里标注的缺口：**角标本身的渲染没有像素证据**。
//
// 办法与它同一套：把 SwiftUI 视图装进 NSHostingView，用进程内的
// `cacheDisplay` 抓帧（不需要屏幕录制权限），并对**颜色通道**做断言：
//   · blocked → 侧栏行里有**橙色**像素（角标 + "为什么停下" 那一行）；
//   · error   → 有**红色**像素；
//   · 对照    —— 同样的布局、只是没有程序状态 → 这些颜色的像素必须消失。
// 没有对照的话，「随便哪里有点橙」都能让断言变绿（假绿）。
import AppKit
@testable import GhostermAppModel
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp

final class TerminalStatusRenderTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 抓帧（与 SidebarChromeTests 同一套）

    private struct Frame {
        let width: Int
        let height: Int
        let pixels: [[(Int, Int, Int)]]

        func pixel(x: Int, y: Int) -> (Int, Int, Int) {
            pixels[min(max(y, 0), height - 1)][min(max(x, 0), width - 1)]
        }

        /// 橙色像素（macOS `.orange` ≈ 255/149/0）。
        ///
        /// `g < 180` 是为了**排除黄色**：连接中的状态点是黄（255/204/0），
        /// 也会落进「红高、蓝低」的朴素判据里——实测对照组因此有 52 个假
        /// 橙色像素。橙色（g≈149）与黄（g≈204）靠 g 就能分开。
        func orangePixels(x0: Int, x1: Int, y0: Int, y1: Int) -> Int {
            count(x0: x0, x1: x1, y0: y0, y1: y1) { r, g, b in
                r > 150 && r > g + 25 && g > b + 20 && g < 180
            }
        }

        /// 红色像素（macOS `.red` ≈ 255/59/48）。`g/b < 110` 同样是为了把
        /// 橙（g≈149）挡在外面——否则 blocked 的角标会被当成「红了」。
        func redPixels(x0: Int, x1: Int, y0: Int, y1: Int) -> Int {
            count(x0: x0, x1: x1, y0: y0, y1: y1) { r, g, b in
                r > 150 && g < 110 && b < 110
            }
        }

        private func count(x0: Int, x1: Int, y0: Int, y1: Int,
                           _ match: (Int, Int, Int) -> Bool) -> Int {
            var n = 0
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    let p = pixel(x: x, y: y)
                    if match(p.0, p.1, p.2) { n += 1 }
                }
            }
            return n
        }
    }

    private func render<V: View>(_ view: V, size: NSSize) -> (Frame, Double) {
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        // 离屏抓帧要显式铺底，否则材质抓到全黑（同 SidebarChromeTests）。
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

    /// 侧栏（一行标签）。`status`/`lastCommand` 都为 nil = 对照组：
    /// 同样的布局，只是没有程序状态、也没有命令记录。
    @MainActor
    private func renderSidebar(status: ProgramStatusReport?,
                               lastCommand: LastCommand? = nil) throws -> (Frame, Double) {
        let dir = NSTemporaryDirectory() + "ghosterm-badge-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")

        let pane = TerminalPane()
        let tab = TerminalTab(kind: .localShell, pane: pane)
        model.tabs.add(tab)
        model.tabs.activeTabID = tab.id
        if let status { pane.applyProgramStatus(status) }
        if let lastCommand { pane.applyLastCommandForTesting(lastCommand) }

        var show = true
        let sb = Binding(get: { show }, set: { show = $0 })
        var editing: HostAlias?
        let eb = Binding(get: { editing }, set: { editing = $0 })
        var adding = false
        let ab = Binding(get: { adding }, set: { adding = $0 })

        return render(Sidebar(model: model, tabs: model.tabs,
                              editingHost: eb, addingHost: ab,
                              showsSidebar: sb, onShowSettings: {}),
                      size: NSSize(width: DS.sidebarWidth, height: 300))
    }

    // MARK: - 断言

    @MainActor
    func testBlockedBadgePaintsOrangePixels() throws {
        let (withBadge, scale) = try renderSidebar(
            status: ProgramStatusReport(state: .blocked, kind: .permission,
                                        app: "terraform", message: "Apply?"))
        let (control, _) = try renderSidebar(status: nil)

        // 第一行标签所在的行区（顶带以下；实测行在 y≈118–142，取宽一点）。
        let x0 = Int(4 * scale), x1 = Int((DS.sidebarWidth - 4) * scale)
        let y0 = Int(95 * scale), y1 = Int(185 * scale)

        let n1 = withBadge.orangePixels(x0: x0, x1: x1, y0: y0, y1: y1)
        let n0 = control.orangePixels(x0: x0, x1: x1, y0: y0, y1: y1)

        XCTAssertGreaterThan(n1, 8,
                             "blocked 应在侧栏画出橙色角标/说明（实得 \(n1) 个橙色像素）")
        XCTAssertEqual(n0, 0,
                       "对照：没有程序状态时不该有任何橙色像素（实得 \(n0)）")
    }

    @MainActor
    func testErrorBadgePaintsRedPixels() throws {
        let (withBadge, scale) = try renderSidebar(
            status: ProgramStatusReport(state: .error, app: "cargo", message: "boom"))
        let (control, _) = try renderSidebar(status: nil)

        let x0 = Int(4 * scale), x1 = Int((DS.sidebarWidth - 4) * scale)
        let y0 = Int(95 * scale), y1 = Int(185 * scale)

        let n1 = withBadge.redPixels(x0: x0, x1: x1, y0: y0, y1: y1)
        let n0 = control.redPixels(x0: x0, x1: x1, y0: y0, y1: y1)

        XCTAssertGreaterThan(n1, 4,
                             "error 应在侧栏画出红色角标（实得 \(n1) 个红色像素）")
        XCTAssertEqual(n0, 0,
                       "对照：没有程序状态时不该有任何红色像素（实得 \(n0)）")
    }

    // MARK: - 上一条命令（OSC 133）

    /// (a) 侧栏：**失败**的那条命令要画出红字（对照：没有记录 → 无红）。
    @MainActor
    func testFailedLastCommandPaintsRedInSidebar() throws {
        let failed = LastCommand(commandLine: "make test", exitCode: 2, duration: 1.5)
        let (withCmd, scale) = try renderSidebar(status: nil, lastCommand: failed)
        let (control, _) = try renderSidebar(status: nil)

        let x0 = Int(4 * scale), x1 = Int((DS.sidebarWidth - 4) * scale)
        let y0 = Int(95 * scale), y1 = Int(200 * scale)
        let n1 = withCmd.redPixels(x0: x0, x1: x1, y0: y0, y1: y1)
        let n0 = control.redPixels(x0: x0, x1: x1, y0: y0, y1: y1)

        XCTAssertGreaterThan(n1, 3, "失败命令在侧栏该有红字（实得 \(n1) 个红像素）")
        XCTAssertEqual(n0, 0, "对照：没有命令记录时不该有红像素（实得 \(n0)）")
    }

    /// 成功（退出码 0）**不**用红字——红只表示失败。
    @MainActor
    func testSuccessfulLastCommandStaysUncolouredInSidebar() throws {
        let ok = LastCommand(commandLine: "ls", exitCode: 0, duration: 0.1)
        let (withCmd, scale) = try renderSidebar(status: nil, lastCommand: ok)
        let x0 = Int(4 * scale), x1 = Int((DS.sidebarWidth - 4) * scale)
        let y0 = Int(95 * scale), y1 = Int(200 * scale)
        XCTAssertEqual(withCmd.redPixels(x0: x0, x1: x1, y0: y0, y1: y1), 0,
                       "退出码 0 不该有红字（三态：成功 ≠ 失败）")
    }

    /// (b) 标签胶囊：非 0 退出码 → 一个小红点（对照：无记录 → 无红）。
    @MainActor
    private func renderStrip(lastCommand: LastCommand?) throws -> (Frame, Double) {
        let dir = NSTemporaryDirectory() + "ghosterm-strip-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")
        let pane = TerminalPane()
        let tab = TerminalTab(kind: .localShell, pane: pane)
        model.tabs.add(tab)
        model.tabs.activeTabID = tab.id
        if let lastCommand { pane.applyLastCommandForTesting(lastCommand) }

        var show = false
        let sb = Binding(get: { show }, set: { show = $0 })
        return render(TabStrip(model: model, tabs: model.tabs,
                               showsSidebar: sb, onShowSettings: {}),
                      size: NSSize(width: 420, height: 44))
    }

    @MainActor
    func testFailedLastCommandPaintsRedDotInTabChip() throws {
        let failed = LastCommand(commandLine: "make", exitCode: 2)
        let (withDot, scale) = try renderStrip(lastCommand: failed)
        let (control, _) = try renderStrip(lastCommand: nil)

        let n1 = withDot.redPixels(x0: 0, x1: withDot.width, y0: 0, y1: withDot.height)
        let n0 = control.redPixels(x0: 0, x1: control.width, y0: 0, y1: control.height)
        XCTAssertGreaterThan(n1, 4,
                             "非 0 退出码该在标签胶囊上画一个红点（实得 \(n1)，scale=\(scale)）")
        XCTAssertEqual(n0, 0, "对照：没有命令记录时不该有红点（实得 \(n0)）")
    }

    /// 「没报退出码」不画红点（三态不折叠：未知 ≠ 失败）。
    @MainActor
    func testUnknownExitCodePaintsNoRedDot() throws {
        let unknown = LastCommand(commandLine: "sleep 1")   // exitCode 为 nil
        let (frame, _) = try renderStrip(lastCommand: unknown)
        XCTAssertEqual(frame.redPixels(x0: 0, x1: frame.width, y0: 0, y1: frame.height), 0,
                       "shell 没报退出码 → 未知，不画失败记号")
    }
}

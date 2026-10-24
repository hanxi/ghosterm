// PaneChromeTests — 窗格「间距」与「标题栏」两个偏好的**像素级**断言。
//
// 这两条都是纯外观：改坏了编译照过、单测照绿，只有屏幕上不对——
//   · 间距设了却没留出来（padding 加在了错的层），两格还是贴着；
//   · 标题栏开关没接上，开了也不画（或关了还画）。
// 两条用的尺子不同：间距是**几何**（量真视图树里两块终端面之间空出多少点），
// 标题栏是**像素**（量顶上那条带里画没画东西）。颜色不能用来量间距——浅色
// 外观下窗口底色与窗格底色都是近白，先前按亮度比被变异验证抓出过假绿。
// 抓帧用进程内 `cacheDisplay`（不需要屏幕录制权限），与 SidebarChromeTests 同。
import AppKit
@testable import GhostermAppModel
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp

final class PaneChromeTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 抓帧（与 SidebarChromeTests 同口径）

    private struct Frame {
        let width: Int
        let height: Int
        let pixels: [[(Int, Int, Int)]]

        func pixel(x: Int, y: Int) -> (Int, Int, Int) {
            pixels[min(max(y, 0), height - 1)][min(max(x, 0), width - 1)]
        }

        /// 指定矩形里「像控件」的像素数：明显暗（字形）或带颜色（状态点）。
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

        /// 指定矩形里的平均**色度**（最大通道 − 最小通道）。灰的底色接近 0，
        /// 点了强调色的底色明显大于 0。比亮度稳：亮度随深浅外观整体平移，
        /// 色度不随。
        func meanChroma(x0: Int, x1: Int, y0: Int, y1: Int) -> Double {
            var sum = 0.0, n = 0.0
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    let p = pixel(x: x, y: y)
                    sum += Double(max(p.0, max(p.1, p.2)) - min(p.0, min(p.1, p.2)))
                    n += 1
                }
            }
            return n > 0 ? sum / n : 0
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

    private func render<V: View>(_ view: V, size: NSSize) -> (Frame, Double) {
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(origin: .zero, size: size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        // 离屏抓帧要显式铺底：材质在没有不透明底时抓到全黑。
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

    private static let size = NSSize(width: 900, height: 600)

    /// 左右分屏的一窗；偏好在**建视图之前**写进去（`@AppStorage` 是读一次的）。
    @MainActor
    private func renderSplit(gap: Int, titleBar: Bool) throws -> (Frame, Double) {
        let d = UserDefaults.standard
        let oldGap = d.object(forKey: PaneGap.key)
        let oldBar = d.object(forKey: PaneTitleBar.visibleKey)
        d.set(gap, forKey: PaneGap.key)
        d.set(titleBar, forKey: PaneTitleBar.visibleKey)
        addTeardownBlock {
            d.set(oldGap, forKey: PaneGap.key)
            d.set(oldBar, forKey: PaneTitleBar.visibleKey)
        }

        let dir = workspaceTestTmp + "pane-chrome-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/config", atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: dir + "/config",
                             knownHostsPath: dir + "/known_hosts")
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())
        return render(RootView(model: model, initialShowsSidebar: false), size: Self.size)
    }

    /// 左右分屏，两格都挂上真 surface，返回两块终端面的窗口坐标（按 x 排序）。
    @MainActor
    private func splitHostFrames(gap: Int, sidebar: Bool = false) throws -> [CGRect] {
        try splitRender(gap: gap, sidebar: sidebar).frames
    }

    /// 同上，另外把**宿主视图自己**的窗口坐标一并带回来——要量「窗格离窗口
    /// 顶多远」就得有个参照系，而窗口的 contentRect 在 `.fullSizeContentView`
    /// 下并不等于 styleMask 给的那个矩形。
    @MainActor
    private func splitRender(gap: Int, sidebar: Bool)
        throws -> (frames: [CGRect], host: CGRect) {
        let d = UserDefaults.standard
        let old = d.object(forKey: PaneGap.key)
        let oldTitleBar = d.object(forKey: PaneTitleBar.visibleKey)
        d.set(gap, forKey: PaneGap.key)
        d.set(true, forKey: PaneTitleBar.visibleKey)
        addTeardownBlock {
            d.set(old, forKey: PaneGap.key)
            d.set(oldTitleBar, forKey: PaneTitleBar.visibleKey)
        }

        let dir = workspaceTestTmp + "pane-gap-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/config", atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: dir + "/config",
                             knownHostsPath: dir + "/known_hosts")
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())
        let app = try TerminalApp.shared()
        for p in tab.panes { p.attachForTesting(surface: try TerminalSurface(app: app)) }

        let host = makeRootHostingView(model: model,
                                       initialShowsSidebar: sidebar)
        host.frame = NSRect(origin: .zero, size: Self.size)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        for _ in 0..<20 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }

        var out: [CGRect] = []
        func walk(_ v: NSView) {
            if v is TerminalHostView { out.append(v.convert(v.bounds, to: nil)) }
            for s in v.subviews { walk(s) }
        }
        walk(host)
        return (out.sorted { $0.minX < $1.minX }, host.convert(host.bounds, to: nil))
    }

    /// 终端区里一条**不会撞到状态覆盖层**的横带（覆盖层居中，这里取下方）。
    private func band(_ scale: Double) -> (Int, Int) {
        (Int(430 * scale), Int(520 * scale))
    }

    // MARK: - 1. 间距

    /// 间距是**几何**，就按几何量：两格的终端面之间空出多少点、
    /// 最左那格离窗口边多少点。
    ///
    /// 先前这条是按中线附近的平均亮度比的，变异验证当场抓出假绿——比到的是
    /// 卡片描边（只有间距>0 时才画），把 `padding` 整个拿掉照样绿。而且窗口
    /// 底色与聚焦窗格的底色在浅色外观下都是近白，颜色本来就分不开。
    @MainActor
    func testGapIsRealWhitespaceBetweenPanes() throws {
        let loose = try splitHostFrames(gap: 14)
        XCTAssertEqual(loose.count, 2, "两格各挂一个终端面")
        let looseInner = loose[1].minX - loose[0].maxX
        let looseEdge = loose[0].minX
        // HSplitView 自己那条分隔线也占一点，故给 ±2.5 的余量。
        XCTAssertEqual(looseInner, 14, accuracy: 2.5,
                       "间距 14 时两格之间应空出 14 点（实测 \(looseInner)）")
        XCTAssertEqual(looseEdge, 14, accuracy: 2.5,
                       "格与窗口边之间也是 14 点（实测 \(looseEdge)）")

        let flush = try splitHostFrames(gap: 0)
        XCTAssertEqual(flush.count, 2)
        let flushInner = flush[1].minX - flush[0].maxX
        XCTAssertLessThan(flushInner, 3,
                          "间距「无」时就该贴边，只剩分隔线（实测 \(flushInner)）")
        XCTAssertLessThan(flush[0].minX, 3,
                          "贴边时最左那格紧挨窗口边（实测 \(flush[0].minX)）")
    }

    // MARK: - 2. 标题栏

    /// 开了标题栏，每格顶上那条带里应有内容（状态点 + 进程名/目录 + 按钮）；
    /// 关了就一个像素都不画。
    ///
    /// 指向性：终端区中下部两次渲染的控件像素都应为 0——开关只影响顶上那条带。
    @MainActor
    func testTitleBarTogglePaintsTheTopStrip() throws {
        let (on, scale) = try renderSplit(gap: 0, titleBar: true)
        let (off, _) = try renderSplit(gap: 0, titleBar: false)

        // 终端区从顶栏（+ 分隔线）下面开始；标题栏就是它最上面 26 点。
        let top = Int((WindowChrome.barHeight + 1) * scale)
        let bottom = Int((WindowChrome.barHeight + 1 + PaneTitleBar.height) * scale)
        let x0 = Int(8 * scale), x1 = Int((Self.size.width / 2 - 8) * scale)
        let painted = on.controlPixels(x0: x0, x1: x1, y0: top, y1: bottom)
        let blank = off.controlPixels(x0: x0, x1: x1, y0: top, y1: bottom)
        XCTAssertGreaterThan(painted, 40,
                             "开了窗格标题栏，顶上那条带里应画出内容；实际 \(painted)")
        XCTAssertLessThan(blank, painted / 4,
                          "关了就不该画（关=\(blank)，开=\(painted)）")

        // 控制组：下半部分是终端面，两次都应干净。
        let (by0, by1) = band(scale)
        XCTAssertEqual(on.controlPixels(x0: x0, x1: x1, y0: by0, y1: by1), 0,
                       "标题栏不该把东西画到终端面里")
        XCTAssertEqual(off.controlPixels(x0: x0, x1: x1, y0: by0, y1: by1), 0,
                       "关掉标题栏时终端面也应干净")
    }

    /// 左右分屏（`split: false` 时只一格），**左边那格**是当前窗格；
    /// 按给定外观抓一帧。
    @MainActor
    private func renderFocusSplit(dark: Bool, split: Bool = true) throws
        -> (Frame, Double) {
        let d = UserDefaults.standard
        let oldGap = d.object(forKey: PaneGap.key)
        let oldBar = d.object(forKey: PaneTitleBar.visibleKey)
        d.set(PaneGap.standard, forKey: PaneGap.key)
        d.set(true, forKey: PaneTitleBar.visibleKey)
        // 外观由调用方 `defer` 还原（同 SettingsUITests 的写法）。注意：还原
        // `NSApp.appearance` 并不能把**线程当前外观**也还回去，那条残留会让后
        // 面用例裸取的 `NSColor.xxx.cgColor` 解成暗色——抓帧的 helper 因此一律
        // 在窗口外观下显式解（见本文件 `render`）。
        NSApp.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        addTeardownBlock {
            if let oldGap { d.set(oldGap, forKey: PaneGap.key) }
            else { d.removeObject(forKey: PaneGap.key) }
            if let oldBar { d.set(oldBar, forKey: PaneTitleBar.visibleKey) }
            else { d.removeObject(forKey: PaneTitleBar.visibleKey) }
        }
        let dir = workspaceTestTmp + "pane-focus-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/config", atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: dir + "/config",
                             knownHostsPath: dir + "/known_hosts")
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        if split {
            tab.split(axis: .horizontal, with: TerminalPane())
            tab.activePaneID = tab.panes[0].id
        }
        return render(RootView(model: model, initialShowsSidebar: false),
                      size: Self.size)
    }

    /// 两格标题栏各自的横带（像素坐标，自上而下）。
    private func titleBands(_ scale: Double) -> (y0: Int, y1: Int,
                                                 left: (Int, Int), right: (Int, Int)) {
        let gap = CGFloat(PaneGap.standard)
        let y0 = Int((WindowChrome.barHeight + 1 + gap) * scale)
        let y1 = Int((WindowChrome.barHeight + 1 + gap + PaneTitleBar.height) * scale)
        let half = Self.size.width / 2
        return (y0, y1,
                (Int((gap + 60) * scale), Int((half - 30) * scale)),
                (Int((half + 60) * scale), Int((Self.size.width - gap - 30) * scale)))
    }

    // MARK: - 3. 分屏时「哪格是当前」

    /// 聚焦那格的标题栏点着强调色，非聚焦那格不点——深浅两种外观下都成立。
    ///
    /// 用户实测报回来的：「深色背景下，非当前选中的窗格颜色不太行，不像未
    /// 激活窗格」。终端面上那层 `unfocused-split-opacity` 遮罩盖的是**配置
    /// 色**，默认就是终端背景色本身，同色盖同色只剩文字淡三成，深色主题下
    /// 读不出来。遮罩是上游语义不动（见 SplitDim），区分放到 chrome 上。
    ///
    /// 量的是**色度**不是亮度：亮度随深浅外观整体平移（实测浅色下两条带只
    /// 差 12，深色下差 29），拿亮度当判据必然要为两种外观各写一个阈值——
    /// 那种阈值没人能说清是怎么来的。带色 vs 不带色与底色深浅无关。
    @MainActor
    func testFocusedPaneTitleBarIsTinted() throws {
        defer { NSApp.appearance = nil }
        for dark in [true, false] {
            let (frame, scale) = try renderFocusSplit(dark: dark)
            let b = titleBands(scale)
            let focused = frame.meanChroma(x0: b.left.0, x1: b.left.1,
                                           y0: b.y0, y1: b.y1)
            let unfocused = frame.meanChroma(x0: b.right.0, x1: b.right.1,
                                             y0: b.y0, y1: b.y1)
            XCTAssertGreaterThan(focused, 20,
                                 "\(dark ? "深色" : "浅色")下聚焦那格的标题栏应点着"
                                 + "强调色（实测色度 \(focused)）")
            XCTAssertLessThan(unfocused, 8,
                              "\(dark ? "深色" : "浅色")下非聚焦那格的标题栏应是"
                              + "窗口底色（实测色度 \(unfocused)）")
        }
    }

    /// 只有一格时**不点色**：那时「哪格是当前」不是个问题，点色只是噪音
    /// （同 `PaneCard` 不画单格聚焦描边的口径）。
    @MainActor
    func testLonePaneTitleBarIsNotTinted() throws {
        defer { NSApp.appearance = nil }
        let (frame, scale) = try renderFocusSplit(dark: true, split: false)
        let b = titleBands(scale)
        let lone = frame.meanChroma(x0: b.left.0, x1: b.left.1, y0: b.y0, y1: b.y1)
        XCTAssertLessThan(lone, 8,
                          "没分屏时标题栏不该点强调色（实测色度 \(lone)）")
    }

    // MARK: - 4. 侧栏展开时窗格要顶到窗口顶


    /// 侧栏展开 + 分屏时，窗格上方**不该**多出一条空带。
    ///
    /// 用户实测报回来的：「左侧栏打开时，多窗格的情况下，顶部出现了多余的
    /// 空白区域」。机理是——侧栏展开时顶部标签条被整块移除（判据见
    /// `TabStripVisibility.shows`），终端列于是从窗口 y=0 开始；而多窗格时
    /// 树里多了 `HSplitView`/`VSplitView`（真 `NSSplitView` 桥接），
    /// `RootView` 根上那句 `.ignoresSafeArea(.container, edges: .top)`
    /// 过不去这层 AppKit 桥，split 的子视图又被按窗口安全区（
    /// `.fullSizeContentView` 下的标题栏高度）内缩了一次。
    ///
    /// 侧栏收起时 38pt 的标签条正好盖住这段，所以这个 bug 只在
    /// 「侧栏开 + 分屏」这一格组合里现形——而那一格此前零覆盖。
    ///
    /// 判据刻意写成**绝对值**而不是「两种侧栏状态之差」：后者在两边同时
    /// 多内缩 28pt 时仍然相等，是条假绿。
    @MainActor
    func testSplitPanesReachWindowTopWhenSidebarIsOpen() throws {
        let gap = PaneGap.standard
        let open = try splitRender(gap: gap, sidebar: true)
        XCTAssertEqual(open.frames.count, 2, "两格各挂一个终端面")

        // 终端面顶边离窗口顶多远。侧栏展开 → 没有标签条，于是只该隔着
        // 容器的半个间距 + 窗格自己的半个间距 + 26pt 窗格标题栏。
        let inset = open.host.maxY - open.frames[0].maxY
        let expected = CGFloat(gap) + PaneTitleBar.height
        XCTAssertEqual(inset, expected, accuracy: 2.5,
                       "侧栏展开时窗格应顶到窗口顶（期望离顶 \(expected) 点，"
                       + "实测 \(inset)——多出来的就是用户看到的那条空带）")

        // 对照组：侧栏收起时标签条回来，窗格就该再往下让出 38pt + 分隔线。
        let closed = try splitRender(gap: gap, sidebar: false)
        XCTAssertEqual(closed.frames.count, 2)
        let closedInset = closed.host.maxY - closed.frames[0].maxY
        XCTAssertEqual(closedInset, expected + WindowChrome.barHeight + 1,
                       accuracy: 2.5,
                       "侧栏收起时窗格应让出顶部标签条（实测 \(closedInset)）")
    }
}

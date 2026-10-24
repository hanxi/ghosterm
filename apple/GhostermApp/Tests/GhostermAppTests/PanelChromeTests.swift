// PanelChromeTests — 内嵌文件面板「顶栏」的两条修复（用户实测反馈）。
//
//   · 「顶部按钮超框了」：工具条原来是一行 HStack（不折行、也不报错），
//     面板窄到 260pt 时超出的按钮被画到框外、裁掉。修法是 FlowLayout 折行。
//   · 「有没有关闭按钮」：原先面板只能靠 ⌘⇧E 开合，顶栏没有关闭按钮。
//
// 两条都是「不报错、只在屏幕上看得出」的改动，故要抓到**像素**才作数：
// 把视图装进 NSHostingView 用进程内 `cacheDisplay` 抓帧（无需录屏权限）。
//
// 判据都带**对照**：
//   · 折行 —— 窄面板的顶栏必须比宽面板**更高**（多一行），否则就是没折；
//   · 不裁 —— 折行时右边不许有控件像素顶到框沿（超框的痕迹）；
//   · 关闭按钮 —— 给了 onClose 才有，不给时那片是空的。
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class PanelChromeTests: XCTestCase {
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
                    if isControl(pixel(x: x, y: y)) { n += 1 }
                }
            }
            return n
        }

        /// 指定矩形里最靠下的控件像素的 y（没有则 -1）。用来量顶栏有多高。
        func maxControlY(x0: Int, x1: Int, y0: Int, y1: Int) -> Int {
            var best = -1
            for y in max(y0, 0)..<min(y1, height) {
                for x in max(x0, 0)..<min(x1, width) {
                    if isControl(pixel(x: x, y: y)) { best = max(best, y); break }
                }
            }
            return best
        }

        private func isControl(_ p: (Int, Int, Int)) -> Bool {
            let luma = (p.0 * 299 + p.1 * 587 + p.2 * 114) / 1000
            let mx = max(p.0, max(p.1, p.2)), mn = min(p.0, min(p.1, p.2))
            return luma < 140 || (mx - mn) > 45
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

    /// 空目录 → 面板内容区是空的，抓到的「控件像素」就只来自顶栏。
    @MainActor
    private func makePanel() throws -> FilesPanelModel {
        let dir = NSTemporaryDirectory() + "ghosterm-panel-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: dir))
        panel.navigate(to: dir)
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && panel.entries.isEmpty && panel.errorText == nil {
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return panel
    }

    @MainActor
    private func renderPanel(width: CGFloat, onClose: (() -> Void)? = nil) throws
        -> (Frame, Double) {
        let panel = try makePanel()
        return render(FilesPanelView(panel: panel, onClose: onClose),
                      size: NSSize(width: width, height: 320))
    }

    // MARK: - 1. 折行规则（纯函数，真实工具条宽度）

    /// 排不下必须折行，且每一行都装在可用宽度内、一个控件都不丢。
    ///
    /// 用**真实**的工具条宽度：9 个 28pt 图标 + 一个 96pt 视图切换，
    /// 项间距 4（= DS.Padding.xs），面板内容宽度 = 260 - 2×8 = 244。
    func testFlowLayoutWrapsWithinAvailableWidth() {
        let widths = [CGFloat](repeating: 28, count: 9) + [96]
        let rows = FlowLayout.rows(widths: widths, available: 244, spacing: 4)
        XCTAssertGreaterThan(rows.count, 1, "244pt 里放不下这排按钮 → 必须折行")
        XCTAssertEqual(rows.flatMap { $0 }, Array(0..<widths.count),
                       "折行是**重排**，不是丢项：每个下标都要出现且只出现一次")
        for row in rows {
            let w = row.reduce(CGFloat(0)) { $0 + widths[$1] }
                + CGFloat(max(0, row.count - 1)) * 4
            XCTAssertLessThanOrEqual(w, 244, "每行的宽度都不能超过可用宽度")
        }
    }

    /// 单项比整行还宽：独占一行（不裁、不缩——裁了就看不到它了）。
    func testFlowLayoutGivesOversizeItemItsOwnRow() {
        let rows = FlowLayout.rows(widths: [40, 300, 40], available: 120, spacing: 4)
        XCTAssertEqual(rows, [[0], [1], [2]], "超宽项必须独占一行")
    }

    /// 装得下就不折（别把一行能放的按钮拆开）。
    func testFlowLayoutKeepsSingleRowWhenItFits() {
        let rows = FlowLayout.rows(widths: [28, 28, 28], available: 200, spacing: 4)
        XCTAssertEqual(rows, [[0, 1, 2]])
    }

    /// 接线：宽面板里工具条是**一行**（顶栏矮），窄面板里折成**两行**（顶栏高）。
    ///
    /// 这就是「超框」那条的对照：真去折了，顶栏才会变高。若退回到单行
    /// HStack，两种宽度下顶栏一样高 —— 这条会红。
    @MainActor
    func testHeaderGrowsTallerWhenPanelIsNarrow() throws {
        let (narrow, scale) = try renderPanel(width: 260)
        let (wide, _) = try renderPanel(width: 700)

        let band = Int(200 * scale)
        let nY = narrow.maxControlY(x0: 0, x1: narrow.width, y0: 0, y1: band)
        let wY = wide.maxControlY(x0: 0, x1: wide.width, y0: 0, y1: band)
        XCTAssertGreaterThan(nY, 0, "窄面板顶栏里该有控件像素")
        XCTAssertGreaterThan(nY - wY, Int(20 * scale),
                             "窄面板工具条应折成两行、顶栏明显更高；"
                             + "实得窄 \(nY) vs 宽 \(wY)")
    }

    /// 对照：折行**不裁**——最小宽度下右边不许有控件像素顶到框沿。
    ///
    /// 超框（单行 HStack）时，右边那几个按钮会被裁在框沿上，最右几列会有
    /// 控件像素。折行后最右是 8pt 内边距，应当是干净的。
    @MainActor
    func testNarrowHeaderDoesNotTouchRightEdge() throws {
        let (frame, scale) = try renderPanel(width: 260)
        let band = Int(200 * scale)
        let edge = frame.controlPixels(x0: max(0, frame.width - Int(2 * scale)),
                                       x1: frame.width, y0: 0, y1: band)
        XCTAssertEqual(edge, 0, "最小宽度下顶栏不该有控件顶到右沿（实得 \(edge)）")
    }

    // MARK: - 2. 关闭按钮（「怎么关掉 SFTP 侧边栏」）

    /// 给了 onClose 就要在第一行最右画一个关闭按钮（对照：不给则那片是空的）。
    @MainActor
    func testCloseButtonAppearsOnlyWhenProvided() throws {
        // 用**宽**面板：工具条一行装得下，右上角那块不会混进折行下来的按钮
        // （窄面板里 340 的右沿会被第二行按钮占掉，对照就不干净了）。
        let (withClose, scale) = try renderPanel(width: 700, onClose: {})
        let (without, _) = try renderPanel(width: 700, onClose: nil)

        // 顶栏右上角：宽 40pt、只取工具条那几行（不含下面的路径行——那是文字，
        // 一直铺到右沿，会把这个对照搅浑）。
        let x0 = Int(660 * scale), x1 = Int(700 * scale)
        let y0 = 0, y1 = Int(120 * scale)
        let n1 = withClose.controlPixels(x0: x0, x1: x1, y0: y0, y1: y1)
        let n0 = without.controlPixels(x0: x0, x1: x1, y0: y0, y1: y1)
        XCTAssertGreaterThan(n1, 6,
                             "给了 onClose 就该在最右画出一个关闭按钮（实得 \(n1)）")
        XCTAssertEqual(n0, 0,
                       "对照：不给 onClose 时右上角不该有控件（实得 \(n0)）")
    }

    // MARK: - 3. 右键菜单上的快捷键

    /// 条目右键菜单必须把能用的键**标出来**，而且标的得是 `PanelKeymap`
    /// 真接的那一个（标错 = 菜单承诺一个按了没反应的键，比不标更糟）。
    /// 没有键的那几条（权限 / 拷贝路径）不许凭空多出一个。
    @MainActor
    func testRowContextMenuShowsPanelShortcuts() throws {
        guard #available(macOS 14.0, *) else {
            throw XCTSkip("面板的键只有 macOS 14+ 接得到（见 PanelKeyHandler），13 上刻意不标")
        }
        let root = NSTemporaryDirectory() + "ghosterm-ctxkeys-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: root,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: root) }
        try Data("hi".utf8).write(to: URL(fileURLWithPath: root + "/a.txt"))
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        panel.navigate(to: root)
        let deadline = Date().addingTimeInterval(5)
        while Date() < deadline && panel.entries.isEmpty {
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        XCTAssertFalse(panel.entries.isEmpty, "目录里该列出 a.txt")

        // quickLook 给上，「快速查看」那条才在菜单里。
        let host = NSHostingView(rootView: FilesPanelView(panel: panel,
                                                          quickLook: { _ in }))
        host.frame = NSRect(x: 0, y: 0, width: 420, height: 320)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        for _ in 0..<12 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }

        let menu = try XCTUnwrap(Self.findRowMenu(host), "取不到条目的右键菜单")
        let items = menu.items.filter { !$0.isSeparatorItem }

        // 键位倒推回 PanelKeymap（面板真正接键的那一份）。
        let viaKeymap: [String: PanelAction] = [
            "重命名…": .rename, "信息…": .info, "删除": .delete,
        ]
        let noShortcut: Set<String> = ["权限…", "拷贝路径", "打开", "下载…"]
        var seen: Set<String> = []
        for item in items {
            seen.insert(item.title)
            let mask = item.keyEquivalentModifierMask
            if let want = viaKeymap[item.title] {
                XCTAssertEqual(PanelKeymap.action(key: item.keyEquivalent,
                                                  command: mask.contains(.command),
                                                  shift: mask.contains(.shift)),
                               want,
                               "「\(item.title)」标的键应就是 PanelKeymap 里 \(want) 那一个")
            } else if item.title == "快速查看" {
                // 空格不走 PanelKeymap（由 onKeyPress(.space) 单独接）。
                XCTAssertEqual(item.keyEquivalent, " ", "「快速查看」该标空格")
                XCTAssertTrue(mask.isEmpty, "空格不带修饰键")
            } else if noShortcut.contains(item.title) {
                XCTAssertTrue(item.keyEquivalent.isEmpty,
                              "「\(item.title)」没有快捷键，不该标一个")
            } else {
                XCTFail("菜单里多了一条没登记的项：\(item.title)")
            }
        }
        XCTAssertTrue(seen.isSuperset(of: ["重命名…", "信息…", "删除", "快速查看"]),
                      "带键的那几条都该在菜单里（实得 \(seen)）")
    }

    /// 列表行的右键菜单就挂在那行的 `CellHostingView.menu` 上（实测：不走
    /// `menu(for:)`），所以沿视图树找第一张认得出的（含「重命名…」）。
    private static func findRowMenu(_ view: NSView) -> NSMenu? {
        if let menu = view.menu,
           menu.items.contains(where: { $0.title == "重命名…" }) { return menu }
        for sub in view.subviews {
            if let menu = findRowMenu(sub) { return menu }
        }
        return nil
    }
}

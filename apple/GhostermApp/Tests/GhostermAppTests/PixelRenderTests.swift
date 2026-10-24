// 像素级渲染验证 —— 「终端真的画在屏幕上」这一步的断言。
//
// 为什么这条曾经被记为「做不了」（以及为什么那个结论是错的）：
//   之前的结论是「无屏幕录制权限时抓像素会 SIGSEGV」，于是闸门只到
//   「网格拿到字节 + Metal 层已挂」。复核后发现两件事：
//     1. 真正会崩的是 **CGWindowListCreateImage**（抓别人的窗口，需要权限）；
//        `NSView.bitmapImageRepForCachingDisplay` + `cacheDisplay` 抓**自己
//        进程内**的视图**不需要**那个权限（本机实测无权限下正常出图）；
//     2. 但 bar 被录到 0 的那次，`cacheDisplay` 确实返回了**全 0** 的位图——
//        那不是崩，是「抓到的帧里没内容」，容易被误读成「权限拦下了」。
//   所以这条闸门现在这么定：抓帧要做**内容判别**，并且配一个对照
//   （同一视图、不同转义序列），证明判据认的是终端的绘制结果，不是任何
//   固定颜色或全黑帧。
//
// 这是「像素级」的口径：验证的是**屏幕上的颜色**与我们的转义序列一致。
// 仍不声称的：字形形状、字体渲染质量、抗锯齿细节（那要逐字形比图）。
import AppKit
import GhostermTerminal
import XCTest

final class PixelRenderTests: XCTestCase {
    /// 进程内共享一个 libghostty app（`TerminalApp` 的文档口径就是
    /// 「进程内单例即可」）。每个用例各建各的会让 app 反复创建/释放，
    /// 实测在全量套件里会崩在 xev 的 mach 异步通知上。
    private static func app() throws -> TerminalApp { try TerminalApp.shared() }

    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 抓帧

    /// 一张抓下来的帧：像素的 RGBA 直方图与其尺寸。
    private struct Frame {
        let width: Int
        let height: Int
        /// 键是 "r,g,b,a"，值是出现次数。
        let histogram: [String: Int]

        var total: Int { width * height }

        /// 出现最多的颜色（背景多半是它）。
        var dominant: (key: String, count: Int) {
            let top = histogram.max { $0.value < $1.value }
            return (top?.key ?? "", top?.value ?? 0)
        }

        /// 满足条件的像素数。
        func count(where predicate: (Int, Int, Int) -> Bool) -> Int {
            var n = 0
            for (key, c) in histogram {
                let p = key.split(separator: ",").compactMap { Int($0) }
                guard p.count == 4 else { continue }
                if predicate(p[0], p[1], p[2]) { n += c }
            }
            return n
        }

        /// 红 / 绿主导的像素数（用来判别「终端把那块背景画成了什么色」）。
        ///
        /// 判据刻意宽松（只要求「该通道明显强于另一通道」），因为终端的
        /// 调色板不是纯色：实测 41（红底）落在 (204,102,102)、42（绿底）
        /// 落在 (180,189,105)——都是调色板混过前景色的结果，用「纯红/纯绿」
        /// 阈值会一个都认不出来（第一版就栽在这里）。
        var reddish: Int { count { r, g, b in r > 120 && r > g + 30 && r > b + 30 } }
        var greenish: Int { count { r, g, b in g > 120 && g > b + 30 && r < g } }
        /// 全黑（抓帧失败的典型长相）。
        var black: Int { count { r, g, b in r == 0 && g == 0 && b == 0 } }
    }

    /// 建面 + 喂字节 + 抓帧。`backgroundSGR` 是要验证的底色转义序列。
    private func capture(backgroundSGR: String,
                         text: String = "PIXEL") throws -> Frame {
        let app = try Self.app()
        let size = NSSize(width: 320, height: 160)
        let surface = try TerminalSurface(
            app: app, frame: NSRect(origin: .zero, size: size))
        let window = NSWindow(
            contentRect: NSRect(x: 200, y: 200, width: size.width, height: size.height),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = surface.view
        TestWindows.presentOffscreen(window)
        surface.setSize(width: UInt32(size.width * 2), height: UInt32(size.height * 2))
        surface.feed(Data("\u{1b}[2J\u{1b}[H\(backgroundSGR)\(text)\u{1b}[0m".utf8))

        // 等渲染器把帧交出来（给足机会，超时后抓到什么是什么——
        // 空帧会被下面的断言判红，不会静默通过）。
        for _ in 0..<40 {
            app.tick()
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }

        defer { window.orderOut(nil) }
        guard let rep = surface.view.bitmapImageRepForCachingDisplay(
            in: surface.view.bounds) else {
            throw XCTSkip("本视图拿不到 bitmap 缓存（环境限制）")
        }
        surface.view.cacheDisplay(in: surface.view.bounds, to: rep)
        guard let cg = rep.cgImage else {
            throw XCTSkip("缓存位图没有 CGImage")
        }

        let w = cg.width, h = cg.height
        var buf = [UInt8](repeating: 0, count: w * h * 4)
        buf.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(
                data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
            ) else { return }
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        var hist: [String: Int] = [:]
        var i = 0
        while i + 3 < buf.count {
            hist["\(buf[i]),\(buf[i+1]),\(buf[i+2]),\(buf[i+3])", default: 0] += 1
            i += 4
        }
        return Frame(width: w, height: h, histogram: hist)
    }

    // MARK: - 断言

    /// 红色背景必须真的出现在屏幕像素上，且**不是**任何固定颜色的巧合：
    /// 同一布局换成绿色背景时，红色像素必须几乎消失。
    func testTerminalBackgroundReachesScreenPixels() throws {
        let red = try capture(backgroundSGR: "\u{1b}[41;97m")
        let green = try capture(backgroundSGR: "\u{1b}[42;97m")

        // 抓帧本身要有效：全黑说明这一帧没内容（早先被误读成「权限拦下了」）。
        XCTAssertLessThan(red.black, red.total / 2,
                          "抓到的帧几乎全黑——这一帧没内容，像素断言无从谈起")

        XCTAssertGreaterThan(red.reddish, red.total / 100,
                             "红色背景应占可观的像素（得到 \(red.reddish)/\(red.total)）")
        XCTAssertGreaterThan(red.reddish, green.reddish * 5,
                             "换绿色背景后红色像素应基本消失"
                             + "（红 \(red.reddish) vs 绿 \(green.reddish)）——"
                             + "这条对照证明判据认的是终端绘制结果，不是固定颜色")
        XCTAssertGreaterThan(green.greenish, green.total / 100,
                             "绿色背景同理应占可观像素（得到 \(green.greenish)）")
    }

    /// 背景色是**主题的背景**而不是系统窗口底色：libghostty 默认主题
    /// 背景不是纯黑（实测 (41,44,52)），且它是画面里最大的一块。
    func testDominantColorIsTerminalThemeBackground() throws {
        let frame = try capture(backgroundSGR: "")
        let top = frame.dominant
        XCTAssertGreaterThan(top.count, frame.total / 2,
                             "终端背景应占画面一半以上（得到 \(top.count)/\(frame.total)）")
        let parts = top.key.split(separator: ",").compactMap { Int($0) }
        XCTAssertEqual(parts.count, 4)
        XCTAssertFalse(parts[0] == 0 && parts[1] == 0 && parts[2] == 0,
                       "主色不该是纯黑——纯黑更像「帧是空的」或系统窗口底色，"
                       + "而不是 libghostty 画的主题背景")
    }

    /// 白字（97）必须出现：前景与背景是两条独立的绘制路径。
    func testForegroundTextIsDrawn() throws {
        let frame = try capture(backgroundSGR: "\u{1b}[41;97m", text: "WWWW")
        let bright = frame.count { r, g, b in r > 200 && g > 200 && b > 200 }
        XCTAssertGreaterThan(bright, 0,
                             "白色前景像素应存在（得到 \(bright)）——"
                             + "只有背景色说明前景绘制路径没通")
    }
}

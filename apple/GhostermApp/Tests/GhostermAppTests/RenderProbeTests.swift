// 渲染冒烟：字节进网格之后，libghostty 是否**真的挂上了渲染面**。
//
// 本测试只到「网格 + 渲染层已挂」这一档；**像素级**验证在
// `PixelRenderTests`（那里真的抓帧比对颜色，并配对照与变异）。
// 两者分工：本文件证明渲染路径接通（层的存在与几何），
// PixelRenderTests 证明画出来的**颜色**与我们的转义序列一致。
//
// 一条踩过的坑记在这里：早先以为「无屏幕录制权限抓像素必崩」——实际崩的
// 只有 CGWindowListCreateImage（抓别人的窗口）；抓**自己进程内**的视图用
// `bitmapImageRepForCachingDisplay` 不需要那个权限（本机实测无权限下正常）。
import AppKit
import GhostermTerminal
import XCTest

final class RenderProbeTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    func testBytesReachGridAndRenderLayerIsAttached() throws {
        let termApp = try TerminalApp()
        let surface = try TerminalSurface(
            app: termApp, frame: NSRect(x: 0, y: 0, width: 480, height: 240)
        )
        let window = NSWindow(
            contentRect: NSRect(x: 400, y: 400, width: 480, height: 240),
            styleMask: [.titled], backing: .buffered, defer: false
        )
        window.contentView = surface.view
        TestWindows.presentOffscreen(window)
        surface.setSize(width: 960, height: 480)
        surface.feed(Data("\u{1b}[2J\u{1b}[H\u{1b}[41;97mGHOSTERM_RED_BLOCK\u{1b}[0m\r\n".utf8))

        // 渲染线程 spawn 在 surface 初始化之后，等它把层挂上。
        for _ in 0..<30 {
            termApp.tick()
            surface.draw()
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
            if surface.view.layer != nil { break }
        }

        let text = surface.readText() ?? ""
        XCTAssertTrue(text.contains("GHOSTERM_RED_BLOCK"),
                      "灌入的文本应出现在网格里（得到: \(text.debugDescription)）")

        guard let layer = surface.view.layer else {
            return XCTFail("surface 的 NSView 上没有层——libghostty 的渲染面未接通")
        }
        XCTAssertTrue(surface.view.wantsLayer,
                      "libghostty 应把 view 置为 layer-hosting")
        XCTAssertGreaterThan(layer.bounds.width, 0, "渲染层应有宽度")
        XCTAssertGreaterThan(layer.bounds.height, 0, "渲染层应有高度")
        XCTAssertGreaterThan(layer.contentsScale, 0, "渲染层应设置 contentsScale（Retina）")

        window.orderOut(nil)
    }
}
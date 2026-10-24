// AboutWindowTests — 「关于」从设置里搬出来、变成一张独立窗口之后的护栏。
//
// 这件事有两条**编译照过、界面照画**的坏法：
//   · 菜单项退回系统标准面板（`orderFrontStandardAboutPanel`）——点得动、
//     也弹窗，但弹的是只认 Info.plist 的那张，随包组件的许可整个没了，
//     而随包分发第三方组件就得把这些声明亮给用户看；
//   · 窗口里那张组件许可表没画出来——窗口在、是空的。
// 两条各有一条断言钉住。变异验证见 `scripts/mutation-check-band-controls.sh`。
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class AboutWindowTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        TestWindows.enterAutomationMode()
    }

    @MainActor
    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "ghosterm-about-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n  HostName h\n".write(toFile: dir + "/config",
                                             atomically: true, encoding: .utf8)
        return isolatedAppModel(configPath: dir + "/config",
                        knownHostsPath: dir + "/known_hosts",
                        settings: SettingsStore(terminalConfigPath: dir + "/ghostty.conf"))
    }

    // MARK: - 菜单项开的是我们自己的窗口

    /// 点「关于 Ghosterm」必须开出**我们这张**窗口。
    ///
    /// 退回系统标准面板的话：菜单项仍然有 action、点了也真的弹窗，前面
    /// `AppMenuCoverageTests` 那两条（action / target 不为 nil）全都挡不住
    /// ——只有「`AboutWindow` 这张窗口出现了」能分辨。
    @MainActor
    func testAboutMenuItemOpensOurOwnWindow() throws {
        let model = try makeModel()
        AppMenu.install(model: model)
        defer { AboutWindow.close(); NSApp.mainMenu = nil }

        var found: NSMenuItem?
        func walk(_ menu: NSMenu) {
            for item in menu.items {
                if item.title == "关于 Ghosterm" { found = item }
                if let sub = item.submenu { walk(sub) }
            }
        }
        if let main = NSApp.mainMenu { walk(main) }
        let about = try XCTUnwrap(found, "主菜单里没有「关于 Ghosterm」")

        let target = try XCTUnwrap(about.target as? NSObject,
                                   "「关于 Ghosterm」没有 target——那它开的多半是"
                                   + "系统标准面板，列不出随包组件的许可")
        let action = try XCTUnwrap(about.action)
        XCTAssertTrue(target.responds(to: action), "关于的 target 不认识它的 action")
        target.perform(action, with: about)

        let window = try XCTUnwrap(AboutWindow.controller?.window,
                                   "点了「关于 Ghosterm」却没有我们的关于窗口")
        XCTAssertTrue(window.isVisible, "关于窗口建出来了但没显示")
        XCTAssertEqual(window.title, "关于 Ghosterm")
    }

    // MARK: - 窗口内容

    /// 随包组件表：名字齐、每条都有许可说明。
    ///
    /// （原在 `SettingsUITests`——「关于」那一页搬成独立窗口时跟着搬过来。）
    func testAboutInfoListsBundledComponents() {
        let names = AboutInfo.entries.map(\.name)
        XCTAssertTrue(names.contains("Ghosterm"))
        XCTAssertTrue(names.contains("libghostty (Ghostty)"))
        XCTAssertTrue(names.contains("libssh2"))
        for entry in AboutInfo.entries {
            XCTAssertFalse(entry.detail.isEmpty, "\(entry.name) 缺许可说明")
        }
    }

    /// 窗口里真的画出了那张组件许可表（不是一张空窗）。
    ///
    /// 判据是**内容高度**加上**非白像素**：表有多少条，视图就得有多高——
    /// 只断言「画了点东西」的话，光一个图标加版本号也能过。
    @MainActor
    func testAboutWindowRendersTheLicenseTable() throws {
        let host = NSHostingView(rootView: AboutContentView())
        host.frame = NSRect(x: 0, y: 0, width: AboutContentView.width, height: 600)
        host.layoutSubtreeIfNeeded()

        // 图标 72 + 版本两行 + 每条组件一行（至少 20pt）：表整个没画出来时
        // 高度会掉到这条线以下。
        let floorHeight = 150 + CGFloat(AboutInfo.entries.count) * 20
        XCTAssertGreaterThan(host.fittingSize.height, floorHeight,
                             "关于窗口内容只有 \(host.fittingSize.height)pt——"
                             + "\(AboutInfo.entries.count) 条组件许可多半没画出来")

        guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else {
            throw XCTSkip("拿不到位图缓存（环境限制）")
        }
        host.cacheDisplay(in: host.bounds, to: rep)
        var nonWhite = 0
        for y in stride(from: 0, to: rep.pixelsHigh, by: 3) {
            for x in stride(from: 0, to: rep.pixelsWide, by: 3) {
                guard let c = rep.colorAt(x: x, y: y)?
                    .usingColorSpace(.deviceRGB) else { continue }
                let luma = c.redComponent * 0.3 + c.greenComponent * 0.59
                    + c.blueComponent * 0.11
                if luma < 0.75 { nonWhite += 1 }
            }
        }
        XCTAssertGreaterThan(nonWhite, 20,
                             "关于窗口画出来几乎是空白（非白像素 \(nonWhite)）")
    }
}

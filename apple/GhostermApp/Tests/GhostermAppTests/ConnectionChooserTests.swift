import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor
final class ConnectionChooserTests: XCTestCase {
    override class func setUp() {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        TestWindows.enterAutomationMode()
    }

    private func makeModel() throws -> AppModel {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: dir) }
        let config = dir.appendingPathComponent("config")
        try "Host web\n  HostName example.test\n\nHost *\n  ServerAliveInterval 60\n".write(to: config, atomically: true, encoding: .utf8)
        return isolatedAppModel(configPath: config.path,
                                knownHostsPath: dir.appendingPathComponent("known_hosts").path,
                                settings: SettingsStore(terminalConfigPath: dir.appendingPathComponent("ghostty").path))
    }

    func testSearchMatchesAllTermsWithoutChangingHostAliases() {
        XCTAssertTrue(ConnectionSearch.matches("WEB deploy", text: "web deploy@example.test"))
        XCTAssertTrue(ConnectionSearch.matches("cafe", text: "café"))
        XCTAssertFalse(ConnectionSearch.matches("web missing", text: "web deploy@example.test"))
        XCTAssertTrue(ConnectionSearch.matches("  ", text: "web"))
    }

    func testLocalFilesAlwaysOpenNewTabAndStaleHostsAreRejected() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        try ConnectionRequest(target: .localShell, kind: .files).open(in: model)
        let first = try XCTUnwrap(model.tabs.activeTab)
        XCTAssertTrue(first.panes.isEmpty)
        XCTAssertNotNil(first.localFiles, "文件请求必须打开文件窗格")
        try ConnectionRequest(target: .localShell, kind: .files).open(in: model)
        XCTAssertEqual(model.tabs.tabs.count, 2, "重复打开文件仍须新建标签")
        XCTAssertFalse(model.tabs.activeTab === first)
        for target in ["removed", "*"] {
            for kind in ConnectionRequest.Kind.allCases {
                XCTAssertThrowsError(try ConnectionRequest(target: .host(target), kind: kind).open(in: model),
                                     "失效或通配别名不得建立连接")
            }
        }
        XCTAssertEqual(model.tabs.tabs.count, 2)
        XCTAssertNil(model.tmuxChooser)
    }

    func testReadFailureClearsStaleHostsAndIsNotAnEmptyConfig() throws {
        let model = try makeModel()
        XCTAssertEqual(model.wildcards, ["*"])
        // 用目录替代配置文件，制造真实读取失败，再由同一入口重试。
        try FileManager.default.removeItem(atPath: model.configPath)
        try FileManager.default.createDirectory(atPath: model.configPath, withIntermediateDirectories: false)
        model.reload()
        XCTAssertNotNil(model.loadError, "读取失败不能报告成空配置")
        XCTAssertTrue(model.hosts.isEmpty)
        XCTAssertTrue(model.wildcards.isEmpty, "读取失败必须清掉旧的通配块")
        XCTAssertThrowsError(try model.applyEdits(alias: "*", [("User", ["stale"])]))
        try FileManager.default.removeItem(atPath: model.configPath)
        model.reload()
        XCTAssertNil(model.loadError, "尚未创建配置是合法空态")
        XCTAssertTrue(model.hosts.isEmpty)
        try model.addHost(alias: "new-host", [("HostName", ["example.test"])])
        XCTAssertEqual(model.hosts.map(\.pattern), ["new-host"], "失败后的重试仍可新增配置")
    }

    func testReadErrorIsVisibleInBothChooserAndHostManager() throws {
        let model = try makeModel()
        func orangePixels<V: View>(_ view: V) throws -> Int {
            let host = NSHostingView(rootView: view)
            host.appearance = NSAppearance(named: .aqua)
            host.frame = NSRect(x: 0, y: 0, width: 620, height: 550)
            host.layoutSubtreeIfNeeded()
            let rep = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: rep)
            var count = 0
            for y in 0..<rep.pixelsHigh {
                for x in stride(from: 0, to: rep.pixelsWide, by: 2) {
                    guard let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                    if c.redComponent > 0.7 && c.greenComponent > 0.2 && c.greenComponent < 0.8 && c.blueComponent < 0.2 { count += 1 }
                }
            }
            return count
        }
        let baseline = try orangePixels(ConnectionChooser(model: model, choose: { _ in }, manageHosts: {}))
        try FileManager.default.removeItem(atPath: model.configPath)
        try FileManager.default.createDirectory(atPath: model.configPath, withIntermediateDirectories: false)
        model.reload()
        XCTAssertNotNil(model.loadError)
        let chooser = try orangePixels(ConnectionChooser(model: model, choose: { _ in }, manageHosts: {}))
        let manager = try orangePixels(HostManagerView(model: model))
        XCTAssertGreaterThan(chooser, baseline + 20, "连接选择器必须画出读取错误")
        XCTAssertGreaterThan(manager, baseline + 20, "主机管理必须画出读取错误")
    }

    func testConnectionAndHostManagementMenuActionsPresentSheets() throws {
        let model = try makeModel()
        AppMenu.install(model: model)
        defer { NSApp.mainMenu = nil }
        let fileMenu = try XCTUnwrap(NSApp.mainMenu?.items.first(where: { $0.submenu?.title == "文件" })?.submenu)
        for title in ["新建连接…", "管理 SSH 配置…"] {
            let item = try XCTUnwrap(fileMenu.items.first { $0.title == title }, "主机管理入口必须保留")
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 600),
                                  styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: false)
            window.isReleasedWhenClosed = false
            window.contentView = makeRootHostingView(model: model)
            TestWindows.presentOffscreen(window)
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
            let target = try XCTUnwrap(item.target as? NSObject)
            target.perform(try XCTUnwrap(item.action), with: item)
            RunLoop.main.run(until: Date().addingTimeInterval(0.6))
            XCTAssertNotNil(window.attachedSheet, "菜单必须打开对应面板：\(title)")
            if let sheet = window.attachedSheet { window.endSheet(sheet); sheet.orderOut(nil) }
            window.orderOut(nil)
            window.contentView = nil
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }
    }
}

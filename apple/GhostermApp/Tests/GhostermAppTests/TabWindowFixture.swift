import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class TabWindowFixture {
    let directory: URL
    let model: AppModel
    let controller: TabWindowController
    var window: NSWindow { controller.window! }
    var root: NSView { window.contentView! }

    init(requestClose: @escaping () -> Void = {}) throws {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        model = isolatedAppModel(configPath: directory.appendingPathComponent("config").path,
                                 knownHostsPath: directory.appendingPathComponent("known_hosts").path,
                                 settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        controller = TabWindowController(model: model, requestClose: requestClose)
        window.setContentSize(NSSize(width: 1000, height: 600))
        TestWindows.presentOffscreenAsKey(window)
        WindowChrome.redress(window)
        settle()
    }
    func capture(_ name: String) throws {
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let output = project.appendingPathComponent("tmp/tab-chrome-screenshots")
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let image = try XCTUnwrap(root.bitmapImageRepForCachingDisplay(in: root.bounds))
        root.cacheDisplay(in: root.bounds, to: image)
        try XCTUnwrap(image.representation(using: .png, properties: [:])).write(to: output.appendingPathComponent(name + ".png"))
    }
    func close() {
        model.dismissTmux(); model.tabs.cancelClose(); model.tabs.removeAll()
        if let sheet = window.attachedSheet { window.endSheet(sheet); sheet.orderOut(nil) }
        window.orderOut(nil)
        window.contentView = nil
    }
    func settle(_ duration: TimeInterval = 0.2) { RunLoop.main.run(until: Date().addingTimeInterval(duration)) }
    func until(_ condition: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(4)
        while Date() < deadline { if condition() { return true }; settle(0.03) }
        return condition()
    }
    func views<T: NSView>(_ type: T.Type, in root: NSView? = nil) -> [T] {
        let view = root ?? self.root
        return ((view as? T).map { [$0] } ?? []) + view.subviews.flatMap { views(type, in: $0) }
    }
    func tabView(_ id: UUID) throws -> HoverTrackingView {
        try XCTUnwrap(views(HoverTrackingView.self).first { $0.identifier?.rawValue == "tab-\(id)" })
    }
    func click(_ point: CGPoint) throws {
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                                       timestamp: ProcessInfo.processInfo.systemUptime,
                                                       windowNumber: window.windowNumber, context: nil,
                                                       eventNumber: 1, clickCount: 1, pressure: 1))
            window.sendEvent(event)
        }
        settle()
    }
    func menu(for id: UUID) throws -> NSMenu {
        let view = try tabView(id)
        let point = view.convert(NSPoint(x: view.bounds.midX, y: view.bounds.midY), to: nil)
        let event = try XCTUnwrap(NSEvent.mouseEvent(with: .rightMouseDown, location: point, modifierFlags: [],
                                                   timestamp: ProcessInfo.processInfo.systemUptime,
                                                   windowNumber: window.windowNumber, context: nil,
                                                   eventNumber: 1, clickCount: 1, pressure: 1))
        func find(_ view: NSView) -> NSMenu? {
            if let menu = view.menu(for: event), !menu.items.isEmpty { return menu }
            return view.subviews.lazy.compactMap { find($0) }.first
        }
        return try XCTUnwrap(find(root), "标签实际命中范围内应提供右键菜单")
    }
    func shell(_ title: String) throws -> (TerminalTab, TerminalSurface) {
        let pane = TerminalPane()
        let surface = try TerminalSurface(app: TerminalApp.shared())
        let session = try SessionController.localShell(surface: surface, executable: "/bin/sh", argv: ["-i"],
                                                       cwd: directory.path, kind: .direct)
        pane.attach(surface: surface, session: session)
        pane.start()
        let tab = TerminalTab(kind: .localShell, pane: pane)
        tab.title = title
        model.tabs.add(tab)
        return (tab, surface)
    }
    func terminalHost(for pane: TerminalPane) throws -> TerminalHostView {
        let workspace = try XCTUnwrap(views(WorkspaceHostView.self).first { $0.hosts[pane.id] != nil })
        return try XCTUnwrap(views(TerminalHostView.self, in: try XCTUnwrap(workspace.hosts[pane.id])).first)
    }
}

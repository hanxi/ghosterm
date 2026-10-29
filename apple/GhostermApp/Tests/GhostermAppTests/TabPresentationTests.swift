import AppKit
import Combine
import GhostermAppModel
import XCTest

@MainActor
final class TabPresentationTests: XCTestCase {
    private func defaults() throws -> UserDefaults {
        let suite = "Ghosterm.TabPresentationTests." + UUID().uuidString
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        addTeardownBlock { defaults.removePersistentDomain(forName: suite) }
        return defaults
    }

    func testPlacementPersistsAndInvalidValueFallsBackToSidebar() throws {
        let store = try defaults()
        var preferences = AppPreferences(defaults: store)
        XCTAssertEqual(preferences.tabPlacement, .sidebar)
        preferences.tabPlacement = .top
        XCTAssertEqual(AppPreferences(defaults: store).tabPlacement, .top, "标签位置必须保存")
        store.set("unsupported", forKey: AppPreferences.Key.tabPlacement.rawValue)
        XCTAssertEqual(preferences.tabPlacement, .sidebar, "非法值使用侧边默认布局")
    }

    func testPlacementUsesIndependentStore() throws {
        let store = try defaults()
        var preferences = AppPreferences(defaults: store)
        preferences.tabPlacement = .top
        XCTAssertEqual(AppPreferences(defaults: try defaults()).tabPlacement, .sidebar,
                       "窗口应读取指定存储")
    }

    func testModelPublishesLayoutChangesWithoutChangingTabs() throws {
        _ = NSApplication.shared
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let store = try defaults()
        let model = AppModel(configPath: dir.appendingPathComponent("config").path,
                             knownHostsPath: dir.appendingPathComponent("known_hosts").path,
                             settings: SettingsStore(terminalConfigPath: dir.appendingPathComponent("ghostty").path),
                             defaults: store)
        let tab = TerminalTab(kind: .localFiles)
        model.tabs.add(tab)
        defer { model.tabs.removeAll() }
        var changes = 0
        let observation = model.objectWillChange.sink { changes += 1 }
        model.preferences.tabPlacement = .top
        XCTAssertEqual(changes, 1, "布局设置必须立即通知已有视图")
        XCTAssertEqual(model.preferences.tabPlacement, .top)
        model.toggleTabPlacement()
        XCTAssertEqual(model.preferences.tabPlacement, .sidebar, "布局命令写同一份偏好")
        XCTAssertTrue(model.tabs.activeTab === tab, "改布局不替换标签对象")
        XCTAssertEqual(model.tabs.tabs.count, 1)
        withExtendedLifetime(observation) {}
    }
}

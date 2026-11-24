import XCTest
@testable import GhostermAppModel

final class PluginCatalogTests: XCTestCase {
    func testFeaturedEntriesUseOfficialRepositoryAndPluginNames() throws {
        XCTAssertEqual(PluginCatalog.builtIn.map(\.source.repositoryPath), ["hanxi/ghosterm", "hanxi/ghosterm"])
        XCTAssertEqual(PluginCatalog.builtIn.map(\.source.plugin), ["preview", "table"])
    }
}

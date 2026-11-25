import Foundation
import XCTest
@testable import GhostermAppModel

final class PluginCatalogTests: XCTestCase {
    func testFeaturedEntriesUseOfficialRepositoryAndPluginNames() throws {
        XCTAssertEqual(PluginCatalog.builtIn.map(\.source.repositoryPath), ["hanxi/ghosterm", "hanxi/ghosterm"])
        XCTAssertEqual(PluginCatalog.builtIn.map(\.source.plugin), ["preview", "table"])
    }

    func testConnectionFailureReportsHostAndCode() async throws {
        let session = makeSession()
        defer { session.invalidateAndCancel() }
        let client = PluginGitHubClient(session: session)
        let source = try PluginGitHubSource("hanxi/offline")

        do {
            _ = try await client.fetchIndex(for: source)
            XCTFail("Connection failure should stop the index read")
        } catch {
            XCTAssertEqual(error as? PluginGitHubClient.Failure,
                           .network(host: "github.com", code: .cannotConnectToHost))
            XCTAssertTrue(error.localizedDescription.contains("github.com"))
            XCTAssertTrue(error.localizedDescription.contains("-1004"))
            XCTAssertFalse(error.localizedDescription.contains("UserInfo"))
        }
    }

    func testIndexNotFoundDescribesTheRequestedRelease() async throws {
        let session = makeSession()
        defer { session.invalidateAndCancel() }
        let client = PluginGitHubClient(session: session)

        for source in [PluginCatalog.builtIn[0].source, try PluginGitHubSource("hanxi/ghosterm@v1")] {
            do {
                _ = try await client.fetchIndex(for: source)
                XCTFail("HTTP 404 should stop the index read")
            } catch {
                XCTAssertEqual(error as? PluginGitHubClient.Failure,
                               .indexNotFound(repository: "hanxi/ghosterm", tag: source.tag))
                XCTAssertTrue(error.localizedDescription.contains("plugin-index.json"))
                XCTAssertTrue(error.localizedDescription.contains(source.tag ?? "最新正式 Release"))
            }
        }
    }

    private func makeSession() -> URLSession {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [PluginCatalogURLProtocol.self]
        return URLSession(configuration: configuration)
    }
}

private final class PluginCatalogURLProtocol: URLProtocol {
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        guard let url = request.url, url.host == "github.com" else {
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
            return
        }
        switch url.path {
        case "/hanxi/offline/releases/latest/download/plugin-index.json":
            client?.urlProtocol(self, didFailWithError: URLError(.cannotConnectToHost))
        case "/hanxi/ghosterm/releases/latest/download/plugin-index.json",
             "/hanxi/ghosterm/releases/download/v1/plugin-index.json":
            let response = HTTPURLResponse(url: url, statusCode: 404, httpVersion: nil, headerFields: nil)!
            client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
            client?.urlProtocolDidFinishLoading(self)
        default:
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
        }
    }

    override func stopLoading() {}
}

import AppKit
import Combine
import CoreGraphics
import GhostermAppModel
import ImageIO
import SwiftUI
import XCTest
import Vision
import WebKit
@testable import GhostermApp

final class PluginResourceUITests: XCTestCase {
    @MainActor
    func testHTMLWebViewUsesFreshNonpersistentStorage() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let handler = PluginResourceHTMLSchemeHandler(snapshot: try await snapshot(main, resources: [main]))
        let first = PluginResourceHTMLWebViewConfiguration.make(schemeHandler: handler)
        let second = PluginResourceHTMLWebViewConfiguration.make(schemeHandler: handler)
        XCTAssertTrue(!first.websiteDataStore.isPersistent && !second.websiteDataStore.isPersistent &&
                      first.websiteDataStore !== second.websiteDataStore,
                      "HTML previews require a fresh nonpersistent data store per view")
    }

    @MainActor
    func testHTMLCoordinatorCloseStopsLoadingAndRejectsLateUpdates() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let prepared = try await snapshot(main, resources: [main])
        let owner = PluginResourceHTMLCoordinator(snapshot: prepared)
        let web = HTMLLoadSpy(frame: .zero, configuration: PluginResourceHTMLWebViewConfiguration.make(schemeHandler: owner.schemeHandler))
        web.navigationDelegate = owner; web.uiDelegate = owner
        owner.update(snapshot: prepared, in: web)
        owner.close(in: web)
        owner.close(in: web)
        let late = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("late document".utf8))
        owner.update(snapshot: try await snapshot(late, resources: [late]), in: web)
        XCTAssertTrue(web.documents == [prepared.document] && web.stops == 1 &&
                      web.navigationDelegate == nil && web.uiDelegate == nil,
                      "closed HTML coordinator must stop once, detach delegates and reject late loads")
    }

    @MainActor
    func testHTMLClosedSchemeHandlerRejectsLateUpdateAndRequest() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let image = resource("image", path: "site/logo.png", mime: "image/png", bytes: try solidRedPNG())
        let handler = PluginResourceHTMLSchemeHandler(snapshot: try await snapshot(main, resources: [main, image]))
        handler.close()
        handler.update(snapshot: try await snapshot(main, resources: [main, image]))
        let task = CapturedSchemeTask(request: URLRequest(url: try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/logo.png"))))
        let configuration = PluginResourceHTMLWebViewConfiguration.make(schemeHandler: handler)
        handler.webView(WKWebView(frame: .zero, configuration: configuration), start: task)
        XCTAssertTrue(task.error != nil && task.response == nil && task.data.isEmpty && !task.finished,
                      "closed HTML scheme handler must reject future reads and late updates")
    }

    @MainActor
    func testHTMLCoordinatorCloseRevokesRetainedSchemeHandler() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let image = resource("image", path: "site/logo.png", mime: "image/png", bytes: try solidRedPNG())
        let owner = PluginResourceHTMLCoordinator(snapshot: try await snapshot(main, resources: [main, image]))
        let web = HTMLLoadSpy(frame: .zero, configuration: PluginResourceHTMLWebViewConfiguration.make(schemeHandler: owner.schemeHandler))
        owner.close(in: web)
        let task = CapturedSchemeTask(request: URLRequest(url: try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/logo.png"))))
        owner.schemeHandler.webView(web, start: task)
        XCTAssertTrue(task.error != nil && task.data.isEmpty && task.response == nil,
                      "coordinator close must revoke its retained attachment handler")
    }

    @MainActor
    func testHTMLRendererExitReportsOnceAndRevokesResources() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let image = resource("image", path: "site/logo.png", mime: "image/png", bytes: try solidRedPNG())
        let prepared = try await snapshot(main, resources: [main, image])
        let task = CapturedSchemeTask(request: URLRequest(url: try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/logo.png"))))
        var reports = 0
        let owner = PluginResourceHTMLCoordinator(snapshot: prepared) { reports += 1 }
        let web = HTMLLoadSpy(frame: .zero, configuration: PluginResourceHTMLWebViewConfiguration.make(schemeHandler: owner.schemeHandler))
        web.navigationDelegate = owner
        owner.update(snapshot: prepared, in: web)
        owner.webViewWebContentProcessDidTerminate(web)
        owner.webViewWebContentProcessDidTerminate(web)
        owner.schemeHandler.webView(web, start: task)
        XCTAssertEqual(reports, 1)
        XCTAssertEqual(web.stops, 1)
        XCTAssertNil(web.navigationDelegate)
        XCTAssertNotNil(task.error)
        XCTAssertTrue(task.data.isEmpty)
    }

    @MainActor
    private final class HTMLLoadSpy: WKWebView {
        var documents: [String] = []
        var stops = 0
        override func loadHTMLString(_ string: String, baseURL: URL?) -> WKNavigation? {
            documents.append(string); return nil
        }
        override func stopLoading() { stops += 1 }
    }

    func testAlreadyCancelledPreviewDoesNotStartDecoding() async {
        let gate = DecodeGate()
        let decode = Task {
            withUnsafeCurrentTask { $0?.cancel() }
            return await PluginResourceDecodeTask.run {
                await gate.recordStart()
                return "unneeded document"
            }
        }
        let result = await decode.value
        let started = await gate.workerStarted()
        XCTAssertNil(result)
        XCTAssertFalse(started, "a cancelled preview must not start decoding")
    }

    func testCancelledPreviewDecodeDiscardsLateResult() async {
        let gate = DecodeGate()
        let decode = Task {
            await PluginResourceDecodeTask.run { await gate.decode() }
        }
        await gate.waitUntilStarted()
        decode.cancel()
        await gate.finish(with: "stale document")
        let result = await decode.value
        let workerWasCancelled = await gate.workerObservedCancellation()
        XCTAssertNil(result, "a late decode must not update a replaced preview")
        XCTAssertTrue(workerWasCancelled, "the detached decode must receive cancellation")
    }

    func testResourceSelectionReconcilesAppendRemovalAndRunStop() {
        let first = resource("first")
        let second = resource("second")
        var selection = PluginResourceSelection()

        selection.reconcile([first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID)
        selection.select(second.resourceID, available: [first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, second.resourceID)
        selection.reconcile([first.resourceID, second.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, second.resourceID, "arrival should preserve the user's selection")
        selection.reconcile([first.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, first.resourceID, "removing the selected resource should select a live item")
        selection.select("stale", available: [first.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID, "a late stale selection must be ignored")
        selection.reconcile([])
        XCTAssertNil(selection.resourceID, "stopping a run must clear its resource selection")
    }

    @MainActor
    func testHTMLPreparedAttachmentArrivalRebuildsAndRevokesTheOldView() async throws {
        TestWindows.enterAutomationMode()
        let main = resource("main", path: "site/index.html", mime: "text/html",
                            bytes: Data("<link rel='stylesheet' href='style.css'><img src='logo.png'>".utf8))
        let css = resource("css", path: "site/style.css", mime: "text/css", bytes: Data("body{color:red}".utf8))
        let image = resource("image", path: "site/logo.png", mime: "image/png", bytes: try solidRedPNG())
        let second = resource("second", path: "other/index.html", mime: "text/html",
                              bytes: Data("<link rel='stylesheet' href='style.css'>".utf8))
        let secondCSS = resource("second-css", path: "other/style.css", mime: "text/css",
                                 bytes: Data("body{color:blue}".utf8))
        let model = ResourceBrowserHarnessModel(resources: [main, css, second, secondCSS])
        model.selection.reconcile(model.resources.map(\.resourceID))
        let host = NSHostingView(rootView: ResourceBrowserHarness(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 900),
                              styleMask: .borderless, backing: .buffered, defer: false)
        window.contentView = host; TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil); window.contentView = nil }
        func web(_ view: NSView) -> WKWebView? {
            if let found = view as? WKWebView { return found }
            return view.subviews.lazy.compactMap(web).first
        }
        func waitForView(otherThan previous: WKWebView? = nil) async throws -> WKWebView? {
            for _ in 0..<100 {
                host.layoutSubtreeIfNeeded()
                if let current = web(host), current !== previous { return current }
                try await Task.sleep(for: .milliseconds(50))
            }
            return nil
        }
        let initial = try await waitForView()
        let first = try XCTUnwrap(initial)
        let handler = try XCTUnwrap(first.configuration.urlSchemeHandler(forURLScheme: "ghosterm-resource") as? PluginResourceHTMLSchemeHandler)
        let request = URLRequest(url: try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(css.displayName)))
        let before = CapturedSchemeTask(request: request); handler.webView(first, start: before)
        XCTAssertEqual(before.data, css.bytes)
        model.resources.append(image)
        let returned = try await waitForView(otherThan: first)
        let after = CapturedSchemeTask(request: request); handler.webView(first, start: after)
        XCTAssertTrue(returned != nil && first.navigationDelegate == nil && after.error != nil && after.data.isEmpty,
                      "prepared attachment changes must replace and revoke the previous HTML view")
        let prepared = try XCTUnwrap(returned)
        let preparedHandler = try XCTUnwrap(prepared.configuration.urlSchemeHandler(
            forURLScheme: "ghosterm-resource") as? PluginResourceHTMLSchemeHandler)
        model.selection.select(second.resourceID, available: model.resources.map(\.resourceID))
        let replacement = try await waitForView(otherThan: prepared)
        let selectedWebView = try XCTUnwrap(replacement)
        let selectedHandler = try XCTUnwrap(selectedWebView.configuration.urlSchemeHandler(
            forURLScheme: "ghosterm-resource") as? PluginResourceHTMLSchemeHandler)
        let revoked = CapturedSchemeTask(request: request)
        preparedHandler.webView(prepared, start: revoked)
        XCTAssertNotNil(revoked.error)
        XCTAssertTrue(revoked.data.isEmpty)
        XCTAssertNil(prepared.navigationDelegate)
        let outOfScope = CapturedSchemeTask(request: request)
        selectedHandler.webView(selectedWebView, start: outOfScope)
        XCTAssertNotNil(outOfScope.error)
        XCTAssertTrue(outOfScope.data.isEmpty)
        let selectedRequest = URLRequest(url: try XCTUnwrap(
            PluginResourceHTMLAssetResolver.documentURL(secondCSS.displayName)))
        let allowed = CapturedSchemeTask(request: selectedRequest)
        selectedHandler.webView(selectedWebView, start: allowed)
        XCTAssertEqual(allowed.data, secondCSS.bytes)

        model.resources = []
        for _ in 0..<100 where web(host) != nil { try await Task.sleep(for: .milliseconds(25)) }
        XCTAssertNil(web(host))
        XCTAssertEqual(window.alphaValue, 0)
        XCTAssertFalse(NSScreen.screens.contains { $0.frame.intersects(window.frame) })
    }

    @MainActor
    func testResourceBrowserSelectionTracksRemovalAndRunStop() async throws {
        let first = resource("first", path: "first.txt", mime: "text/plain",
                             bytes: Data("first-preview-token".utf8))
        let second = resource("second", path: "second.txt", mime: "text/plain",
                              bytes: Data("second-preview-token".utf8))
        let model = ResourceBrowserHarnessModel(resources: [first, second])
        model.selection.reconcile([first.resourceID, second.resourceID])
        let host = NSHostingView(rootView: ResourceBrowserHarness(model: model))
        host.frame = NSRect(x: 0, y: 0, width: 900, height: 440)

        func waitForPreview(_ expected: String, absent: String? = nil) async throws {
            for _ in 0..<40 {
                host.layoutSubtreeIfNeeded()
                if let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                    host.cacheDisplay(in: host.bounds, to: bitmap)
                    let text = try recognizedText(in: bitmap)
                    if text.contains(expected) && (absent.map { !text.contains($0) } ?? true) {
                        return
                    }
                }
                try await Task.sleep(for: .milliseconds(50))
            }
            XCTFail("expected selected preview text \(expected); current selection: \(String(describing: model.selection.resourceID))")
        }

        try await waitForPreview("first-preview-token", absent: "second-preview-token")
        model.selection.select(second.resourceID, available: [first.resourceID, second.resourceID])
        try await waitForPreview("second-preview-token", absent: "first-preview-token")

        model.resources = [first]
        try await waitForPreview("first-preview-token", absent: "second-preview-token")
        XCTAssertEqual(model.selection.resourceID, first.resourceID,
                       "removing the selected snapshot must reconcile to a live resource")

        model.resources = []
        for _ in 0..<40 where model.selection.resourceID != nil {
            host.layoutSubtreeIfNeeded()
            try await Task.sleep(for: .milliseconds(25))
        }
        XCTAssertNil(model.selection.resourceID, "stopping the run must clear the selected resource")
        host.layoutSubtreeIfNeeded()
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        let text = try recognizedText(in: bitmap)
        XCTAssertFalse(text.contains("first-preview-token"))
        XCTAssertFalse(text.contains("second-preview-token"))
    }

    @MainActor
    func testResourceBrowserRendersASelectedVerifiedResource() throws {
        let item = resource("first")
        let view = PluginResourceBrowser(resources: [item],
                                         selection: .constant(PluginResourceSelection(resourceID: item.resourceID)))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 720, height: 420)
        host.layoutSubtreeIfNeeded()
        XCTAssertGreaterThan(host.fittingSize.width, 0)
        XCTAssertGreaterThan(host.fittingSize.height, 0)
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        var visible = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 3) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 3) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent < 0.9 || color.greenComponent < 0.9 || color.blueComponent < 0.9 {
                    visible += 1
                }
            }
        }
        XCTAssertGreaterThan(visible, 20, "selected resource and preview should render nonempty content")
    }

    @MainActor
    func testMarkdownPreviewDrawsAnExplicitImageSnapshot() async throws {
        let markdownBytes = Data("Before ![red sample](images/sample.png) after".utf8)
        let imageBytes = try solidRedPNG()
        let markdown = resource("markdown", path: "site/index.md", mime: "text/markdown",
                                bytes: markdownBytes)
        let image = resource("sample", path: "site/images/sample.png", mime: "image/png",
                             bytes: imageBytes)
        let view = PluginResourceBrowser(resources: [markdown, image],
            selection: .constant(PluginResourceSelection(resourceID: markdown.resourceID)))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 720, height: 420)

        for _ in 0..<40 {
            host.layoutSubtreeIfNeeded()
            if let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) {
                host.cacheDisplay(in: host.bounds, to: bitmap)
                if redPixelCount(in: bitmap) > 10 { return }
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        XCTFail("the explicitly received Markdown image should render in the selected preview")
    }

    func testHTMLAssetResolverServesOnlyExplicitSameDirectoryBundleResources() throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("<img src='images/logo.png'>".utf8))
        let stylesheet = resource("css", path: "site/styles/site.css", mime: "text/css", bytes: Data("body { color: red }".utf8))
        let image = resource("image", path: "site/images/logo.png", mime: "image/png", bytes: Data([1, 2, 3]))
        let siblingEscape = resource("escape", path: "site2/images/logo.png", mime: "image/png", bytes: Data([4]))
        let nextDocument = resource("next", path: "site/next.html", mime: "text/html", bytes: Data("next".utf8))
        let laterAsset = resource("later", path: "site/later.png", mime: "image/png", bytes: Data([5]))
        let resources = [main, stylesheet, image, siblingEscape, nextDocument, laterAsset]

        XCTAssertEqual(PluginResourcePreviewAssets.after(main, in: resources).map(\.resourceID),
                       [stylesheet.resourceID, image.resourceID])
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: resources)
        let cssURL = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/styles/site.css"))
        XCTAssertEqual(resolver.resolve(cssURL)?.mime, "text/css")
        XCTAssertEqual(resolver.resolve(cssURL)?.data, stylesheet.bytes)
        let imageURL = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/images/logo.png"))
        XCTAssertEqual(resolver.resolve(imageURL)?.data, image.bytes)
        XCTAssertNil(resolver.resolve(URL(string: "https://example.invalid/site/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://other/site/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site2/images/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/later.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/../site2/images/logo.png")!))
        XCTAssertNil(PluginResourceHTMLAssetResolver.documentURL("site/../index.html"))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/blocked.svg")!))
    }

    func testHTMLResolverKeepsCanonicallyEquivalentFilenamesDistinct() throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let decomposed = resource("nfd", path: "site/cafe\u{301}.css", mime: "text/css", bytes: Data("decomposed".utf8))
        let composed = resource("nfc", path: "site/café.css", mime: "text/css", bytes: Data("composed".utf8))
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, decomposed, composed])
        let first = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(decomposed.displayName))
        let second = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(composed.displayName))
        XCTAssertTrue(resolver.resolve(first)?.data == decomposed.bytes && resolver.resolve(second)?.data == composed.bytes,
                      "different UTF-8 filenames must keep their own snapshot bytes")
    }

    func testHTMLResolverRejectsEquivalentButDifferentDirectoryBytes() throws {
        let main = resource("main", path: "site/cafe\u{301}/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let outside = resource("outside", path: "site/café/theme.css", mime: "text/css", bytes: Data("outside".utf8))
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, outside])
        let url = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(outside.displayName))
        XCTAssertTrue(PluginResourcePreviewAssets.after(main, in: [main, outside]).isEmpty && resolver.resolve(url) == nil,
                      "equivalent Unicode directories must not widen the exact document scope")
    }

    func testHTMLCanonicalPathsUseSlashBytesDespiteFollowingCombiningMarks() {
        let invalid = ["/\u{301}.html", "site//\u{301}.html", "site/\u{301}/../bad.html"]
        XCTAssertTrue(invalid.allSatisfy { PluginResourceHTMLAssetResolver.documentURL($0) == nil },
                      "combining marks must not hide absolute or empty path components")
    }

    func testHTMLResolverRejectsMalformedUtf8WithoutAliasingReplacementNames() throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let replacement = resource("replacement", path: "site/�.css", mime: "text/css", bytes: Data("replacement".utf8))
        let literal = resource("literal", path: "site/%FF.css", mime: "text/css", bytes: Data("literal percent".utf8))
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, replacement, literal])
        let valid = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(replacement.displayName))
        let percent = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(literal.displayName))
        let malformed = try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/%FF.css"))
        XCTAssertTrue(resolver.resolve(valid)?.data == replacement.bytes &&
                      resolver.resolve(percent)?.data == literal.bytes && resolver.resolve(malformed) == nil,
                      "malformed UTF-8 must not alias a valid replacement filename or decode percent twice")
    }

    func testHTMLResolverPreservesCombiningMarksAdjacentToSlashBytes() throws {
        let main = resource("main", path: "\u{301}folder/\u{301}index.html", mime: "text/html", bytes: Data("doc".utf8))
        let css = resource("css", path: "\u{301}folder/\u{301}theme.css", mime: "text/css", bytes: Data("exact".utf8))
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, css])
        let base = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL(main.displayName))
        let url = try XCTUnwrap(URL(string: "%CC%81theme.css", relativeTo: base)?.absoluteURL)
        XCTAssertTrue(base.absoluteString == "ghosterm-resource://preview/%CC%81folder/%CC%81index.html" &&
                      resolver.resolve(url)?.data == css.bytes,
                      "valid combining marks after slash bytes must retain their exact path and attachment")
    }

    func testHTMLAssetResolverRejectsDuplicateInvalidAndOverLimitSnapshots() {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("doc".utf8))
        let duplicateA = resource("duplicate-a", path: "site/logo.png", mime: "image/png", bytes: Data([1]))
        let duplicateB = resource("duplicate-b", path: "site/logo.png", mime: "image/png", bytes: Data([2]))
        let badLength = ExtensionController.ReceivedResource(resourceID: "bad-length",
            sha256: String(repeating: "c", count: 64), mime: "image/png",
            displayName: "site/bad-length.png", size: 2, bytes: Data([3]))
        let oversized = resource("oversized", path: "site/oversized.png", mime: "image/png",
                                 bytes: Data(repeating: 4, count: 1_048_577))
        let firstAllowed = resource("first", path: "site/first.png", mime: "image/png", bytes: Data([5]))
        let secondAllowed = resource("second", path: "site/second.png", mime: "image/png", bytes: Data([6]))
        let beyondSix = resource("seventh", path: "site/seventh.png", mime: "image/png", bytes: Data([7]))
        let resources = [main, duplicateA, duplicateB, badLength, oversized,
                         firstAllowed, secondAllowed, beyondSix]
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: resources)

        XCTAssertEqual(PluginResourcePreviewAssets.after(main, in: resources).count, 6)
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/bad-length.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/oversized.png")!))
        XCTAssertNotNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/first.png")!))
        XCTAssertNotNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/second.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/seventh.png")!))
    }

    func testMarkdownImageResolvesOnlyToAnExplicitScopedSnapshot() throws {
        let source = Data("![logo](images/logo.png)".utf8)
        let main = resource("markdown", path: "site/index.md", mime: "text/markdown", bytes: source)
        let imageData = Data([1, 2, 3])
        let image = resource("logo", path: "site/images/logo.png", mime: "image/png", bytes: imageData)
        let markdown = try XCTUnwrap(PluginResourceMarkdownDecoder.decode(
            source, mime: main.mime, displayName: main.displayName,
            baseURL: PluginResourceHTMLAssetResolver.documentURL(main.displayName)))
        let imageURL = try XCTUnwrap(markdown.runs.first(where: { $0.imageURL != nil })?.imageURL?.absoluteURL)
        let resolver = PluginResourceHTMLAssetResolver(document: main, resources: [main, image])

        XCTAssertEqual(resolver.resolve(imageURL)?.data, imageData)
        XCTAssertEqual(resolver.resolve(imageURL)?.mime, "image/png")
        XCTAssertNil(resolver.resolve(URL(string: "https://example.invalid/logo.png")!))
        XCTAssertNil(resolver.resolve(URL(string: "ghosterm-resource://preview/site/other.png")!))
    }

    @MainActor
    func testHTMLSchemeHandlerReturnsTheVerifiedSnapshotBytes() async throws {
        let main = resource("main", path: "site/index.html", mime: "text/html", bytes: Data("<img src='logo.png'>".utf8))
        let image = resource("image", path: "site/style.css", mime: "text/css", bytes: Data("body{color:red}".utf8))
        let handler = PluginResourceHTMLSchemeHandler(snapshot: try await snapshot(main, resources: [main, image]))
        let request = URLRequest(url: try XCTUnwrap(URL(string: "ghosterm-resource://preview/site/style.css")))
        let task = CapturedSchemeTask(request: request)

        handler.webView(WKWebView(frame: .zero), start: task)

        XCTAssertEqual(task.response?.mimeType, "text/css")
        XCTAssertEqual((task.response as? HTTPURLResponse)?.value(forHTTPHeaderField: "Cache-Control"),
                       "no-store, no-cache, must-revalidate")
        XCTAssertEqual(task.data, image.bytes)
        XCTAssertTrue(task.finished)
        XCTAssertNil(task.error)
    }

    func testHTMLNavigationPolicyAllowsOnlyTheCurrentStaticDocument() throws {
        let document = try XCTUnwrap(PluginResourceHTMLAssetResolver.documentURL("site/index.html"))
        XCTAssertTrue(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .other, isMainFrame: true, url: document, documentURL: document))
        XCTAssertTrue(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .other, isMainFrame: true, url: URL(string: "about:blank"), documentURL: document))
        XCTAssertTrue(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .reload, isMainFrame: true, url: document, documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .linkActivated, isMainFrame: true,
            url: URL(string: "https://example.invalid"), documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .other, isMainFrame: false, url: document, documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsAction(
            type: .other, isMainFrame: true,
            url: URL(string: "ghosterm-resource://preview/site/other.html"), documentURL: document))
        XCTAssertTrue(PluginResourceHTMLNavigationPolicy.permitsResponse(
            url: document, isMainFrame: true, documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsResponse(
            url: URL(string: "https://example.invalid"), isMainFrame: true,
            documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsResponse(
            url: document, isMainFrame: false, documentURL: document))
        XCTAssertFalse(PluginResourceHTMLNavigationPolicy.permitsResponse(
            url: nil, isMainFrame: true, documentURL: nil))
    }

    private func snapshot(_ document: ExtensionController.ReceivedResource,
                          resources: [ExtensionController.ReceivedResource]) async throws -> PluginResourceHTMLSnapshot {
        let value = await PluginResourceHTMLSnapshot.prepare(document: document, resources: resources)
        return try XCTUnwrap(value)
    }

    private func resource(_ id: String) -> ExtensionController.ReceivedResource {
        resource(id, path: "\(id).txt", mime: "text/plain", bytes: Data("preview \(id)".utf8))
    }

    private func resource(_ id: String, path: String, mime: String,
                          bytes: Data) -> ExtensionController.ReceivedResource {
        return ExtensionController.ReceivedResource(resourceID: id,
            sha256: String(repeating: "a", count: 64), mime: mime,
            displayName: path, size: UInt64(bytes.count), bytes: bytes)
    }

    private func solidRedPNG() throws -> Data {
        let pixels = Data((0..<16 * 16).flatMap { _ in [UInt8(0xff), 0, 0, 0xff] })
        let provider = try XCTUnwrap(CGDataProvider(data: pixels as CFData))
        let image = try XCTUnwrap(CGImage(width: 16, height: 16, bitsPerComponent: 8,
            bitsPerPixel: 32, bytesPerRow: 64, space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue), provider: provider,
            decode: nil, shouldInterpolate: false, intent: .defaultIntent))
        let encoded = NSMutableData()
        let destination = try XCTUnwrap(CGImageDestinationCreateWithData(encoded, "public.png" as CFString, 1, nil))
        CGImageDestinationAddImage(destination, image, nil)
        XCTAssertTrue(CGImageDestinationFinalize(destination))
        return encoded as Data
    }

    private func redPixelCount(in bitmap: NSBitmapImageRep) -> Int {
        var count = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 2) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 2) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent > 0.8 && color.greenComponent < 0.3 && color.blueComponent < 0.3 {
                    count += 1
                }
            }
        }
        return count
    }

    private func recognizedText(in bitmap: NSBitmapImageRep) throws -> String {
        let image = try XCTUnwrap(bitmap.cgImage)
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.usesLanguageCorrection = false
        try VNImageRequestHandler(cgImage: image).perform([request])
        return (request.results ?? []).compactMap { $0.topCandidates(1).first?.string }
            .joined(separator: " ")
    }

    private final class CapturedSchemeTask: NSObject, WKURLSchemeTask {
        let request: URLRequest
        private(set) var response: URLResponse?
        private(set) var data = Data()
        private(set) var finished = false
        private(set) var error: Error?

        init(request: URLRequest) { self.request = request }
        func didReceive(_ response: URLResponse) { self.response = response }
        func didReceive(_ data: Data) { self.data.append(data) }
        func didFinish() { finished = true }
        func didFailWithError(_ error: Error) { self.error = error }
    }

    private actor DecodeGate {
        private var started = false
        private var startWaiter: CheckedContinuation<Void, Never>?
        private var resultWaiter: CheckedContinuation<String, Never>?
        private var cancelledAtReturn = false

        func recordStart() {
            started = true
            startWaiter?.resume()
            startWaiter = nil
        }

        func decode() async -> String {
            recordStart()
            let value = await withCheckedContinuation { resultWaiter = $0 }
            cancelledAtReturn = Task.isCancelled
            return value
        }

        func waitUntilStarted() async {
            guard !started else { return }
            await withCheckedContinuation { startWaiter = $0 }
        }

        func finish(with value: String) {
            resultWaiter?.resume(returning: value)
            resultWaiter = nil
        }

        func workerStarted() -> Bool { started }
        func workerObservedCancellation() -> Bool { cancelledAtReturn }
    }
}

@MainActor
private final class ResourceBrowserHarnessModel: ObservableObject {
    @Published var resources: [ExtensionController.ReceivedResource]
    @Published var selection = PluginResourceSelection()

    init(resources: [ExtensionController.ReceivedResource]) {
        self.resources = resources
    }
}

@MainActor
private struct ResourceBrowserHarness: View {
    @ObservedObject var model: ResourceBrowserHarnessModel

    var body: some View {
        PluginResourceBrowser(resources: model.resources, selection: $model.selection)
    }
}

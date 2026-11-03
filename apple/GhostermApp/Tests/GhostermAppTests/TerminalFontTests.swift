import AppKit
import CoreText
import GhostermAppModel
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp

final class TerminalFontTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
        TestWindows.enterAutomationMode()
    }
    private func directory() throws -> URL {
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let url = project.appendingPathComponent("tmp/mac-fonts-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: url) }
        return url
    }
    @MainActor private func model(config: String = "font-size = 20\n") throws -> (AppModel, URL) {
        let root = try directory()
        let configURL = root.appendingPathComponent("ghostty.conf")
        try config.write(to: configURL, atomically: true, encoding: .utf8)
        try "Host test\n HostName localhost\n".write(to: root.appendingPathComponent("ssh.conf"), atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: root.appendingPathComponent("ssh.conf").path,
                                     knownHostsPath: root.appendingPathComponent("known").path,
                                     settings: SettingsStore(terminalConfigPath: configURL.path))
        XCTAssertNil(model.terminalConfigError)
        return (model, configURL)
    }
    func testBundledFontResolvesToItsVerifiedProcessResourceAndLicense() throws {
        try TerminalFonts.prepare()
        XCTAssertNil(TerminalFonts.issue)
        let expected = try TerminalFonts.validateResources(at: TerminalFonts.resourceDirectory())
        let face = CTFontCreateWithName("SourceCodePro-Regular" as CFString, 20, nil)
        XCTAssertEqual((CTFontCopyAttribute(face, kCTFontURLAttribute) as? URL)?.resolvingSymlinksInPath(), expected.resolvingSymlinksInPath())
        XCTAssertEqual(CTFontCopyFamilyName(face) as String, "Source Code Pro")
        XCTAssertTrue(CTFontGetSymbolicTraits(face).contains(.traitMonoSpace))
        XCTAssertTrue(try TerminalFonts.licenseText().contains("SIL OPEN FONT LICENSE"))
        for style in ["Regular", "Bold", "It", "BoldIt"] {
            let font = CTFontCreateWithName(("SourceCodePro-" + style) as CFString, 20, nil)
            let expected = try TerminalFonts.resourceDirectory().appendingPathComponent("SourceCodePro-\(style).ttf")
            XCTAssertEqual((CTFontCopyAttribute(font, kCTFontURLAttribute) as? URL)?.resolvingSymlinksInPath(), expected.resolvingSymlinksInPath())
            XCTAssertEqual(CTFontGetSymbolicTraits(font).contains(.traitBold), style.contains("Bold"))
            XCTAssertEqual(CTFontGetSymbolicTraits(font).contains(.traitItalic), style.contains("It"))
        }
    }
    func testMissingAndCorruptFontResourcesFailExplicitly() throws {
        let root = try directory().appendingPathComponent("Fonts")
        try FileManager.default.copyItem(at: TerminalFonts.resourceDirectory(), to: root)
        let font = root.appendingPathComponent("SourceCodePro-Regular.ttf")
        try Data([1,2,3]).write(to: font)
        XCTAssertThrowsError(try TerminalFonts.validateResources(at: root))
        try FileManager.default.removeItem(at: font)
        XCTAssertThrowsError(try TerminalFonts.validateResources(at: root))
    }

    func testEveryStyleIsRequiredAndIndividuallyVerified() throws {
        let root = try directory().appendingPathComponent("Fonts")
        try FileManager.default.copyItem(at: TerminalFonts.resourceDirectory(), to: root)
        for style in ["Regular", "Bold", "It", "BoldIt"] {
            let file = root.appendingPathComponent("SourceCodePro-\(style).ttf")
            let original = try Data(contentsOf: file)
            try Data([1,2,3]).write(to: file)
            XCTAssertThrowsError(try TerminalFonts.validateResources(at: root), style)
            try original.write(to: file)
        }
    }
    func testCatalogKeepsEngineDefaultAndOneBundledFaceAndFiltersActualMonospacedFonts() throws {
        let fonts = TerminalFonts.available()
        XCTAssertEqual(fonts.first, .engineDefault)
        XCTAssertNil(fonts.first?.previewName, "Do not pretend a system fallback previews the engine's embedded font")
        XCTAssertEqual(fonts.filter { $0.family == "Source Code Pro" }.count, 1)
        XCTAssertEqual(fonts.first { $0.family == "Source Code Pro" }?.source, .bundled)
        XCTAssertEqual(Set(fonts.map(\.id)).count, fonts.count)
        XCTAssertTrue(fonts.contains { $0.source == .system })
        XCTAssertFalse(fonts.contains { $0.family == "Times" })
        for font in fonts.compactMap(\.family) { XCTAssertNoThrow(try TerminalFonts.validate(family: font), font) }
        XCTAssertEqual(TerminalFonts.filtered(fonts, query: " source CODE ").map(\.family), ["Source Code Pro"])
        XCTAssertTrue(TerminalFonts.filtered(fonts, query: "no-such-font-8b32").isEmpty)
    }
    func testSelectionPresentsQuotedMissingAndFallbackValuesWithoutChangingThem() {
        XCTAssertEqual(TerminalFontSelection(config: "# font-family = Missing\n").title, "默认 · JetBrains Mono")
        XCTAssertEqual(TerminalFontSelection(config: "font-family = \"Menlo\"\n").family, "Menlo")
        XCTAssertEqual(TerminalFontSelection(config: "font-family = Missing Font\n").family, "Missing Font")
        XCTAssertTrue(TerminalFontSelection(config: "font-family = Menlo\nfont-family = Monaco\n").custom)
        XCTAssertTrue(TerminalFontSelection(config: "config-file = extra.conf\n").external)
    }
    func testProportionalDefaultFaceCannotBeSelectedAsMonospace() {
        XCTAssertThrowsError(try TerminalFonts.validate(family: "Times"))
    }
    func testChoosingOneFaceReplacesLocalFallbacksButPreservesUnrelatedBytes() {
        let input = "# keep font-family = Monaco\nfont-family = Menlo\n\tfont-family = Monaco\nfont-family-bold = Menlo Bold\n# unknown comment\nfont-size = 14\n"
        let untouched = "# keep font-family = Monaco\nfont-family-bold = Menlo Bold\n# unknown comment\nfont-size = 14\n"
        XCTAssertEqual(TerminalFontSelection.replacing(in: input, family: nil), untouched)
        XCTAssertEqual(TerminalFontSelection.replacing(in: input, family: "Source Code Pro"), untouched + "font-family = \"Source Code Pro\"\n")
    }
    @MainActor func testApplyPersistsBacksUpAndRestoresSelectionOnReopen() throws {
        let (model, file) = try model()
        let original = try Data(contentsOf: file)
        let before = model.terminalConfigRevision
        try model.selectTerminalFont(family: "Source Code Pro")
        XCTAssertEqual(model.terminalFontSelection.family, "Source Code Pro")
        XCTAssertEqual(model.terminalConfigRevision, before + 1)
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: file.path + ".bak")), original)
        let reopened = isolatedAppModel(configPath: model.configPath, knownHostsPath: model.knownHostsPath,
                                       settings: SettingsStore(terminalConfigPath: file.path))
        XCTAssertNil(reopened.terminalConfigError)
        XCTAssertEqual(reopened.terminalFontSelection.family, "Source Code Pro")
        try reopened.selectTerminalFont(family: nil)
        XCTAssertNil(SettingsStore.value(of: "font-family", in: reopened.terminalConfigText))
    }
    @MainActor func testUnavailableFamilyAndExternalEditDoNotChangeDisk() throws {
        let (model, file) = try model()
        let original = try Data(contentsOf: file)
        XCTAssertThrowsError(try model.selectTerminalFont(family: "not-installed-ghosterm-83aa"))
        XCTAssertThrowsError(try model.selectTerminalFont(family: "Times"))
        XCTAssertThrowsError(try model.selectTerminalFont(family: "Menlo\nfont-size = 90"))
        XCTAssertEqual(try Data(contentsOf: file), original)
        try "# external write\nfont-size = 21\n".write(to: file, atomically: true, encoding: .utf8)
        let external = try Data(contentsOf: file)
        XCTAssertThrowsError(try model.selectTerminalFont(family: "Source Code Pro"))
        XCTAssertEqual(try Data(contentsOf: file), external)
    }
    @MainActor func testAdditionalConfigurationIsNeverSilentlyReplaced() throws {
        let (model, file) = try model(config: "config-file = ?/nonexistent-ghosterm-font-test.conf\n")
        let original = try Data(contentsOf: file)
        XCTAssertThrowsError(try model.selectTerminalFont(family: "Source Code Pro"))
        XCTAssertEqual(try Data(contentsOf: file), original)
    }
    @MainActor func testPickerPreviewIsDraftUntilApplyAndSearchKeepsSelection() throws {
        let (model, file) = try model()
        let original = try Data(contentsOf: file)
        let picker = TerminalFontPickerModel(model: model)
        picker.refresh()
        let deadline = Date().addingTimeInterval(10)
        while picker.loading && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        XCTAssertFalse(picker.loading)
        picker.selection = "Source Code Pro"
        XCTAssertTrue(picker.canApply)
        XCTAssertEqual(picker.selected?.previewName, "SourceCodePro-Regular")
        picker.query = "zz-no-match"
        XCTAssertTrue(picker.visible.isEmpty)
        XCTAssertEqual(picker.selection, "Source Code Pro")
        XCTAssertEqual(try Data(contentsOf: file), original)
        XCTAssertTrue(picker.apply())
        XCTAssertEqual(model.terminalFontSelection.family, "Source Code Pro")
    }
    @MainActor func testPickerNativeViewRendersAndCapturesTheRealFontChoices() throws {
        let (model, _) = try model()
        let host = NSHostingView(rootView: TerminalFontPicker(model: model, dismiss: {}))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 522, height: 512), styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        RunLoop.main.run(until: Date().addingTimeInterval(0.4))
        host.layoutSubtreeIfNeeded()
        let rep = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: rep)
        XCTAssertGreaterThan(rep.pixelsWide, 450)
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let output = project.appendingPathComponent("tmp/macos-fonts/picker.png")
        try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
        try XCTUnwrap(rep.representation(using: .png, properties: [:])).write(to: output)
    }

    @MainActor func testFontChangeRepaintsExistingSurfaceWithoutReplacingTheShell() throws {
        let (model, file) = try model(config: "font-family = Menlo\nfont-size = 24\nbackground = #101010\nforeground = #eeeeee\ncursor-style-blink = false\n")
        let app = try XCTUnwrap(model.terminalApp)
        let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 240))
        let session = try SessionController.localShell(surface: surface, executable: "/bin/sh", argv: ["-i"], cwd: file.deletingLastPathComponent().path, kind: .direct)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 240), styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = surface.view
        TestWindows.presentOffscreen(window)
        surface.setSize(width: 1600, height: 480)
        session.start()
        defer {
            surface.onInput?(Data("exit\r".utf8))
            _ = session.waitUntilExit(timeout: 3)
            session.stop(); window.orderOut(nil)
        }
        let pidFile = file.deletingLastPathComponent().appendingPathComponent("pid")
        func wait(_ condition: () -> Bool) {
            let end = Date().addingTimeInterval(10)
            while Date() < end && !condition() { app.tick(); RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
            XCTAssertTrue(condition(), "Terminal fixture did not become ready")
        }
        surface.onInput?(Data("stty -echo; PS1=''; printf '%s' \"$$\" > pid; printf '\\033[2J\\033[H\\033[?25lFONTCHANGE MWil0O {}[]'\r".utf8))
        wait { (surface.readText() ?? "").contains("FONTCHANGE MWil0O") && FileManager.default.fileExists(atPath: pidFile.path) }
        let pid = try String(contentsOf: pidFile, encoding: .utf8)
        XCTAssertNotNil(Int(pid))
        func pixels(_ name: String) throws -> Data {
            for _ in 0..<25 { app.tick(); surface.draw(); RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
            let rep = try XCTUnwrap(surface.view.bitmapImageRepForCachingDisplay(in: surface.view.bounds))
            surface.view.cacheDisplay(in: surface.view.bounds, to: rep)
            let image = try XCTUnwrap(rep.cgImage)
            var bytes = [UInt8](repeating: 0, count: image.width * image.height * 4)
            bytes.withUnsafeMutableBytes { buffer in
                let context = CGContext(data: buffer.baseAddress, width: image.width, height: image.height, bitsPerComponent: 8,
                                        bytesPerRow: image.width * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
                context.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
            }
            var root = URL(fileURLWithPath: #filePath); for _ in 0..<5 { root.deleteLastPathComponent() }
            let output = root.appendingPathComponent("tmp/macos-fonts/terminal-\(name).png")
            try FileManager.default.createDirectory(at: output.deletingLastPathComponent(), withIntermediateDirectories: true)
            try rep.representation(using: .png, properties: [:])?.write(to: output)
            return Data(bytes)
        }
        let before = try pixels("menlo")
        XCTAssertEqual(before, try pixels("menlo-control"), "Unchanged glyph pixels must be stable before testing a font change")
        try model.selectTerminalFont(family: "Source Code Pro")
        XCTAssertTrue(app === model.terminalApp)
        let after = try pixels("source-code-pro")
        XCTAssertNotEqual(before, after, "The selected family must change actual full libghostty glyph pixels")
        XCTAssertTrue((surface.readText() ?? "").contains("FONTCHANGE MWil0O"))
        surface.onInput?(Data("printf '%s' \"$$\" > pid-after\r".utf8))
        let afterPid = file.deletingLastPathComponent().appendingPathComponent("pid-after")
        wait { FileManager.default.fileExists(atPath: afterPid.path) }
        XCTAssertEqual(try String(contentsOf: afterPid, encoding: .utf8), pid)
        XCTAssertEqual(session.state, .running)
    }
}

import AppKit
import CoreText
import CryptoKit
import Foundation

public struct TerminalFontChoice: Identifiable, Equatable, Sendable {
    public enum Source: String, Sendable { case engine = "引擎内置", bundled = "应用内置", system = "系统字体" }
    public var id: String { family ?? "" }
    public let family: String?
    public let name: String
    public let previewName: String?
    public let source: Source

    public static let engineDefault = TerminalFontChoice(family: nil, name: "默认 · JetBrains Mono", previewName: nil, source: .engine)
}

/// Font discovery and app-private Core Text registration. No system installation or font downloads.
public enum TerminalFonts {
    public static let bundleName = "GhostermApp_GhostermAppModel.bundle"
    private struct Manifest: Decodable {
        struct Face: Decodable { let file: String, postscript: String, sha256: String }
        let family: String, postscript: String, file: String, sha256: String
        let license: String, license_sha256: String
        let faces: [Face]
    }
    public static func resourceDirectory() throws -> URL {
        if Bundle.main.bundleURL.pathExtension == "app" {
            guard let resources = Bundle.main.resourceURL else { throw SettingsStoreError.invalid("应用字体资源目录缺失") }
            guard let bundle = Bundle(url: resources.appendingPathComponent(bundleName)), let base = bundle.resourceURL else {
                throw SettingsStoreError.invalid("随包字体资源缺失")
            }
            let directory = base.appendingPathComponent("Fonts")
            guard FileManager.default.fileExists(atPath: directory.path) else { throw SettingsStoreError.invalid("随包字体资源缺失") }
            return directory
        }
        return Bundle.module.resourceURL!.appendingPathComponent("Fonts")
    }

    private static func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    public static func validateResources(at directory: URL) throws -> URL {
        let manifest = try JSONDecoder().decode(Manifest.self, from: Data(contentsOf: directory.appendingPathComponent("manifest.json")))
        guard manifest.family == "Source Code Pro", manifest.postscript == "SourceCodePro-Regular",
              manifest.file == "SourceCodePro-Regular.ttf", manifest.license == "SourceCodePro-OFL.txt" else {
            throw SettingsStoreError.invalid("随包字体清单无效")
        }
        let file = directory.appendingPathComponent(manifest.file)
        guard digest(try Data(contentsOf: file)) == manifest.sha256,
              digest(try Data(contentsOf: directory.appendingPathComponent(manifest.license))) == manifest.license_sha256 else {
            throw SettingsStoreError.invalid("随包字体或许可校验失败")
        }
        guard Set(manifest.faces.map(\.postscript)) == Set(["SourceCodePro-Regular", "SourceCodePro-Bold", "SourceCodePro-It", "SourceCodePro-BoldIt"]), manifest.faces.count == 4 else {
            throw SettingsStoreError.invalid("随包字体缺少常规、粗体或斜体字形")
        }
        for face in manifest.faces {
            guard face.file == face.postscript + ".ttf",
                  digest(try Data(contentsOf: directory.appendingPathComponent(face.file))) == face.sha256 else {
                throw SettingsStoreError.invalid("随包字体字形校验失败：\(face.postscript)")
            }
        }
        return file
    }

    // One registration per process, before the shared terminal engine is constructed.
    private static let registration: Result<URL, Error> = Result {
        let directory = try resourceDirectory()
        let regular = try validateResources(at: directory)
        let manifest = try JSONDecoder().decode(Manifest.self, from: Data(contentsOf: directory.appendingPathComponent("manifest.json")))
        for face in manifest.faces {
            let url = directory.appendingPathComponent(face.file)
            var error: Unmanaged<CFError>?
            let registered = CTFontManagerRegisterFontsForURL(url as CFURL, .process, &error)
            let registrationError = error?.takeRetainedValue()
            let font = CTFontCreateWithName(face.postscript as CFString, 14, nil)
            let resolved = CTFontCopyAttribute(font, kCTFontURLAttribute) as? URL
            guard (registered || (registrationError.map { CFErrorGetCode($0) == CTFontManagerError.alreadyRegistered.rawValue } ?? false)),
                  resolved?.resolvingSymlinksInPath() == url.resolvingSymlinksInPath() else {
                throw SettingsStoreError.invalid("Source Code Pro 无法注册到当前进程：\(registrationError.map { String(describing: $0) } ?? "字体来源不匹配")")
            }
        }
        return regular
    }
    public static func prepare() throws { _ = try registration.get() }
    public static var issue: String? {
        if case .failure(let error) = registration { return String(describing: error) }; return nil
    }
    public static func licenseText() throws -> String {
        try String(contentsOf: resourceDirectory().appendingPathComponent("SourceCodePro-OFL.txt"), encoding: .utf8)
    }

    public static func available() -> [TerminalFontChoice] {
        _ = registration
        let descriptors = CTFontCollectionCreateMatchingFontDescriptors(CTFontCollectionCreateFromAvailableFonts(nil)) as? [CTFontDescriptor] ?? []
        var families: [String: (TerminalFontChoice, Int)] = [:]
        for descriptor in descriptors {
            let font = CTFontCreateWithFontDescriptor(descriptor, 14, nil)
            let traits = CTFontGetSymbolicTraits(font)
            guard traits.contains(.traitMonoSpace) else { continue }
            let family = CTFontCopyFamilyName(font) as String
            guard !family.hasPrefix("."), !family.isEmpty, !family.contains(where: { $0.isNewline }) else { continue }
            let name = CTFontCopyPostScriptName(font) as String
            let score = (traits.contains(.traitBold) ? 2 : 0) + (traits.contains(.traitItalic) ? 1 : 0)
            let item = TerminalFontChoice(family: family, name: family, previewName: name, source: .system)
            if families[family].map({ score < $0.1 }) ?? true { families[family] = (item, score) }
        }
        let bundled = TerminalFontChoice(family: "Source Code Pro", name: "Source Code Pro", previewName: "SourceCodePro-Regular", source: .bundled)
        if issue == nil { families.removeValue(forKey: bundled.name) }
        let system = families.values.map(\.0).filter { item in
            // A family can mix proportional and fixed-pitch faces (for example Osaka).
            // Only expose families whose default face can actually be selected as monospace.
            guard let family = item.family else { return false }
            return (try? validate(family: family)) != nil
        }
        return [.engineDefault] + (issue == nil ? [bundled] : []) + system.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }
    public static func filtered(_ items: [TerminalFontChoice], query: String) -> [TerminalFontChoice] {
        let query = query.trimmingCharacters(in: .whitespacesAndNewlines)
        return items.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) || $0.source.rawValue.contains(query) }
    }
    public static func validate(family: String) throws {
        guard !family.contains(where: { $0.isNewline }), !family.contains("\""), !family.contains("\\") else {
            throw SettingsStoreError.invalid("字体名称不能包含换行、引号或反斜杠")
        }
        let font = CTFontCreateWithName(family as CFString, 14, nil)
        guard (CTFontCopyFamilyName(font) as String).caseInsensitiveCompare(family) == .orderedSame,
              CTFontGetSymbolicTraits(font).contains(.traitMonoSpace) else {
            throw SettingsStoreError.invalid("字体未安装或不是等宽字体：\(family)")
        }
    }
}

/// A presentation of local declarations, not an alternative Ghostty configuration parser.
public struct TerminalFontSelection: Equatable {
    public let family: String?
    public let custom: Bool
    public let external: Bool
    public var title: String { external ? "由附加配置管理" : custom ? "自定义字体配置" : family ?? TerminalFontChoice.engineDefault.name }
    public init(config: String) {
        let external = SettingsStore.value(of: "config-file", in: config) != nil
        let values = config.components(separatedBy: "\n").compactMap { line -> String? in
            SettingsStore.value(of: "font-family", in: line)
        }
        var family = values.first ?? ""
        if family.hasPrefix("\""), family.hasSuffix("\""), family.count >= 2 { family = String(family.dropFirst().dropLast()) }
        self.external = external
        self.custom = values.count > 1 || family.contains("\\") || family.contains("\"")
        self.family = family.isEmpty || self.custom || external ? nil : family
    }
    public static func replacing(in text: String, family: String?) -> String {
        // font-family is repeatable. Explicitly choosing one face replaces every local fallback declaration.
        var result = text
        while SettingsStore.value(of: "font-family", in: result) != nil {
            result = SettingsStore.merging(result, edits: [("font-family", nil)])
        }
        return SettingsStore.merging(result, edits: [("font-family", family.map { "\"\($0)\"" })])
    }
}

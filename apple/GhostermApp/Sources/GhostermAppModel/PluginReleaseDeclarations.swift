import Foundation

/// Display declarations only from bytes whose raw digest Core attested.
enum PluginReleaseDeclarations {
    static let manifestName = "manifest.json"

    static func decode(_ bytes: Data, manifestDigest: String) throws -> ([ExtensionController.ReleasePermission], String?) {
        guard bytes.count <= 65536 else { throw PluginDirectorySignatureVerifier.Failure.limit }
        guard PluginReleaseCatalog.sha(bytes) == manifestDigest else { throw PluginReleaseCatalog.Failure.changed }
        let manifest = try ReleaseJSON.parse(bytes).object()
        guard try manifest["schemaVersion"]?.integer(maximum: 2) == 2 else {
            throw PluginReleaseCatalog.Failure.invalid
        }
        let permissions = try luaPermissions(manifest)
        let executor: String?
        if manifest["executor"] != nil {
            guard let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any], let value = object["executor"] else { throw PluginReleaseCatalog.Failure.invalid }
            executor = String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]), as: UTF8.self)
        } else { executor = nil }
        return (permissions, executor)
    }

    private static func luaPermissions(_ manifest: [String: ReleaseJSON.Value]) throws -> [ExtensionController.ReleasePermission] {
        guard manifest["client"] != nil || manifest["executor"] != nil else { throw PluginReleaseCatalog.Failure.invalid }
        return try ["client", "executor"].flatMap { side -> [ExtensionController.ReleasePermission] in
            guard let role = try manifest[side]?.object() else { return [] }
            guard let requirements = try role["requires"]?.array() else { throw PluginReleaseCatalog.Failure.invalid }
            let programs = try role["programs"]?.array().map { try $0.string() } ?? []
            return try requirements.map {
                let capability = try $0.string()
                return .init(side: side, capability: capability, purpose: "", scope: "",
                             programs: capability == "process.spawn" ? programs : [])
            }
        }
    }

}

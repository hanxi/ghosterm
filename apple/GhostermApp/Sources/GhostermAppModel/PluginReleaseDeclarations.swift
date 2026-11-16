import Foundation

/// Display declarations only from bytes whose raw digest Core attested.
enum PluginReleaseDeclarations {
    static func decode(_ bytes: Data, manifestDigest: String) throws -> ([ExtensionController.ReleasePermission], String?) {
        guard bytes.count <= 65536 else { throw PluginDirectorySignatureVerifier.Failure.limit }
        guard PluginReleaseCatalog.sha(bytes) == manifestDigest else { throw PluginReleaseCatalog.Failure.changed }
        let manifest = try ReleaseJSON.parse(bytes).object()
        guard let values = try manifest["permissions"]?.array() else { throw PluginReleaseCatalog.Failure.invalid }
        let permissions = try values.map { value -> ExtensionController.ReleasePermission in
            let object = try value.object()
            guard let side = try object["side"]?.string(), let capability = try object["capability"]?.string(),
                  let purpose = try object["purpose"]?.string(), let scope = try object["scope"]?.string() else { throw PluginReleaseCatalog.Failure.invalid }
            let programs = try object["programs"].map { try $0.array().map { try $0.string() } } ?? []
            return .init(side: side, capability: capability, purpose: purpose, scope: scope, programs: programs)
        }
        let executor: String?
        if manifest["executor"] != nil {
            guard let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any], let value = object["executor"] else { throw PluginReleaseCatalog.Failure.invalid }
            executor = String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.prettyPrinted, .sortedKeys]), as: UTF8.self)
        } else { executor = nil }
        return (permissions, executor)
    }
}

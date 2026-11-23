import Foundation

struct PluginGitHubInstallCoordinator {
    struct Material: Sendable {
        let directory: URL
        let archive: URL
        let descriptor: URL
        let signature: URL
    }
    let fileManager: FileManager
    init(fileManager: FileManager = .default) { self.fileManager = fileManager }

    func stage(_ assets: PluginGitHubClient.VerifiedAssets) throws -> Material {
        let root = fileManager.temporaryDirectory.appendingPathComponent("ghosterm-plugin-" + UUID().uuidString, isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        do {
            let archive = root.appendingPathComponent("payload.zip")
            let descriptor = root.appendingPathComponent("release.json")
            let signature = root.appendingPathComponent("release.sig")
            try assets.archive.write(to: archive, options: .atomic)
            try assets.releaseJSON.write(to: descriptor, options: .atomic)
            try assets.releaseSignature.write(to: signature, options: .atomic)
            return Material(directory: root, archive: archive, descriptor: descriptor, signature: signature)
        } catch {
            try? fileManager.removeItem(at: root)
            throw error
        }
    }
    func discard(_ material: Material) { try? fileManager.removeItem(at: material.directory) }
}

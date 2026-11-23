import Foundation

actor PluginTrustStore {
    private let file: URL
    private var values: [PluginTrustRecord] = []
    init(directory: URL) throws {
        self.file = directory.appendingPathComponent("plugin-trust-v1.json")
        if FileManager.default.fileExists(atPath: file.path) {
            let data = try Data(contentsOf: file)
            values = (try? JSONDecoder().decode([PluginTrustRecord].self, from: data))?.filter(\.isValid) ?? []
        }
    }
    func records() -> [PluginTrustRecord] { values }
    func put(_ record: PluginTrustRecord) throws {
        guard record.isValid else { throw CocoaError(.validationMissingMandatoryProperty) }
        values.removeAll { $0.id == record.id }; values.append(record); try persist()
    }
    func remove(id: String) throws { values.removeAll { $0.id == id }; try persist() }
    private func persist() throws {
        let data = try JSONEncoder().encode(values)
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: file, options: .atomic)
    }
}

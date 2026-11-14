// Standalone Native owner fixture; no App, process recovery or user data.
import Darwin
import Foundation

@main struct JournalProbe {
    static func snapshot(_ journal: ExtensionLocalJournal) throws -> [[String: Any]] {
        try journal.inspect().map { entry in
            ["id": entry.id, "snapshot": entry.snapshot, "directory": entry.directory,
             "held": entry.held, "location": entry.location.rawValue, "reservedBytes": entry.reservedBytes]
        }
    }
    static func main() {
        do { try run() }
        catch { fputs("REJECTED: \(error)\n", stderr); exit(2) }
    }
    static func run() throws {
        let args = CommandLine.arguments
        guard args.count >= 3 else { exit(2) }
        let journal = try ExtensionLocalJournal(directory: URL(fileURLWithPath: args[2]))
        if args[1] == "inspect" {
            let data = try JSONSerialization.data(withJSONObject: snapshot(journal), options: .sortedKeys)
            print(String(decoding: data, as: UTF8.self)); return
        }
        if args[1] == "release" {
            guard args.count == 6 else { exit(2) }
            try journal.releaseMissing(id: args[3], snapshot: args[4], confirmed: args[5] == "yes")
            print("released"); return
        }
        guard args.count == 5, ["intent", "namespace"].contains(args[1]) else { exit(2) }
        alarm(8) // Fallback for a failed harness, never accepted as crash cleanup.
        let base = args[3], ready = URL(fileURLWithPath: args[4])
        let fd = open(base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { exit(2) }; defer { Darwin.close(fd) }
        if args[1] == "intent" {
            let id = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
            let lease = try journal.reserve(parent: fd, base: base, id: id)
            try JSONSerialization.data(withJSONObject: snapshot(journal), options: .sortedKeys).write(to: ready)
            withExtendedLifetime(lease) { while true { pause() } }
        } else {
            let owner = try ExtensionLocalTemporaryDirectory(parentFD: fd, base: base, journal: journal)
            try Data("OWNED".utf8).write(to: URL(fileURLWithPath: owner.directory).appendingPathComponent("keep"))
            try JSONSerialization.data(withJSONObject: snapshot(journal), options: .sortedKeys).write(to: ready)
            withExtendedLifetime(owner) { while true { pause() } }
        }
    }
}

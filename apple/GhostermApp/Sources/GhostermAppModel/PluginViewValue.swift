import CoreFoundation
import Foundation

/// Shared strict JSON value boundary for standard Native view contributions.
enum PluginViewValue {
    enum Failure: Error { case invalidResponse, unconfirmedResult }
    static func success(_ data: Data) throws -> [String: Any] {
        guard data.count <= 3072, String(data: data, encoding: .utf8) != nil else { throw Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw Failure.invalidResponse }
        guard envelope["kind"] as? String == "success" else { throw Failure.unconfirmedResult }
        try keys(envelope, ["kind", "value"])
        guard let value = envelope["value"] as? [String: Any], try jsonBytes(value) <= 2048 else { throw Failure.invalidResponse }
        return value
    }

    static func keys(_ object: [String: Any], _ expected: Set<String>) throws {
        guard Set(object.keys) == expected else { throw Failure.invalidResponse }
    }
    static func identifier(_ value: Any?) throws -> String {
        guard let value = value as? String, (1...64).contains(value.utf8.count),
              let first = value.utf8.first, alphanumeric(first),
              value.utf8.allSatisfy({ alphanumeric($0) || [45, 46, 95].contains($0) }) else { throw Failure.invalidResponse }
        return value
    }
    static func alphanumeric(_ c: UInt8) -> Bool { (48...57).contains(c) || (65...90).contains(c) || (97...122).contains(c) }
    static func revision(_ value: Any?) throws -> String {
        guard let value = value as? String, (1...20).contains(value.utf8.count), value.utf8.first != 48,
              value.utf8.allSatisfy({ (48...57).contains($0) }), let number = UInt64(value), number > 0 else { throw Failure.invalidResponse }
        return value
    }
    static func integer(_ value: Any?, range: ClosedRange<Int>) throws -> Int {
        guard let value = value as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(),
              value.doubleValue >= Double(range.lowerBound), value.doubleValue <= Double(range.upperBound),
              value.doubleValue == Double(value.intValue) else { throw Failure.invalidResponse }
        return value.intValue
    }
    static func text(_ value: Any?, maximum: Int, nonempty: Bool = false) throws -> String {
        guard let value = value as? String, value.unicodeScalars.count <= maximum, !nonempty || !value.isEmpty,
              value.unicodeScalars.allSatisfy({ !((0...8).contains($0.value) || (11...12).contains($0.value) || (14...31).contains($0.value) || $0.value == 127) }) else { throw Failure.invalidResponse }
        return value
    }
    /// JSON.stringify byte accounting, independent of Foundation's optional
    /// slash / Unicode escaping. The raw envelope was already bounded to 3072.
    static func jsonBytes(_ value: Any) throws -> Int {
        if let string = value as? String {
            return 2 + string.unicodeScalars.reduce(0) { size, scalar in
                size + (scalar.value == 34 || scalar.value == 92 || [8, 9, 10, 12, 13].contains(scalar.value) ? 2
                    : scalar.value < 32 ? 6 : scalar.utf8.count)
            }
        }
        if let array = value as? [Any] { return try 2 + max(0, array.count - 1) + array.reduce(0) { try $0 + jsonBytes($1) } }
        if let object = value as? [String: Any] {
            return try 2 + max(0, object.count - 1) + object.reduce(0) { try $0 + jsonBytes($1.key) + 1 + jsonBytes($1.value) }
        }
        if let number = value as? NSNumber {
            if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue ? 4 : 5 }
            // Valid view numbers are bounded integers. Their original JSON
            // spelling (1, 1.0, 1e0) does not change JSON.stringify's size.
            return String(number.intValue).utf8.count
        }
        if value is NSNull { return 4 }
        throw Failure.invalidResponse
    }
}

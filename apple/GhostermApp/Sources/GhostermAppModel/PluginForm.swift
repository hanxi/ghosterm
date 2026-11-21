import CoreFoundation
import Foundation

public struct PluginFormRequest: Identifiable, Equatable, Sendable {
    public struct Field: Identifiable, Equatable, Sendable {
        public struct Option: Identifiable, Equatable, Sendable {
            public let id: String
            public let label: String
        }
        public enum Kind: Equatable, Sendable {
            case text(required: Bool, maxLength: Int)
            case toggle
            case choice([Option])
        }
        public let id: String
        public let label: String
        public let kind: Kind
    }
    public enum Value: Equatable, Sendable {
        case text(String)
        case toggle(Bool)
    }
    public enum Answer: Equatable, Sendable {
        case dismissed
        case submitted([String: Value])
    }

    public let id: String
    public let title: String
    public let fields: [Field]

    public static let method = "view.form.request"
    private static let bodyLimit = 2048
    private typealias Failure = PluginViewValue.Failure

    public static func decode(_ data: Data) throws -> Self {
        guard data.count <= 3072 else { throw Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        guard let value = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              try PluginViewValue.jsonBytes(value) <= bodyLimit,
              let fields = value["fields"] as? [[String: Any]],
              (1...8).contains(fields.count) else { throw Failure.invalidResponse }
        try PluginViewValue.keys(value, ["requestId", "title", "fields"])
        let parsed = try fields.map(decodeField)
        guard Set(parsed.map(\.id)).count == parsed.count else { throw Failure.invalidResponse }
        return .init(id: try PluginViewValue.revision(value["requestId"]), title: try label(value["title"]), fields: parsed)
    }

    public func outcome(_ answer: Answer) throws -> Data {
        var value: [String: Any] = ["requestId": id]
        switch answer {
        case .dismissed:
            value["kind"] = "dismissed"
        case .submitted(let supplied):
            guard supplied.count == fields.count else { throw Failure.invalidResponse }
            var result: [String: Any] = [:]
            for field in fields {
                switch (field.kind, supplied[field.id]) {
                case (.text(let required, let maxLength), .text(let text)):
                    result[field.id] = try Self.checkedText(text, maximum: maxLength, required: required)
                case (.toggle, .toggle(let enabled)):
                    result[field.id] = enabled
                case (.choice(let options), .text(let selected)):
                    guard options.contains(where: { $0.id == selected }) else { throw Failure.invalidResponse }
                    result[field.id] = selected
                default:
                    throw Failure.invalidResponse
                }
            }
            value["kind"] = "submitted"
            value["values"] = result
        }
        guard try PluginViewValue.jsonBytes(value) <= Self.bodyLimit else { throw Failure.invalidResponse }
        return try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value],
                                          options: [.sortedKeys, .withoutEscapingSlashes])
    }

    private static func decodeField(_ field: [String: Any]) throws -> Field {
        let common: Set<String> = ["kind", "id", "label"]
        let kind: Field.Kind
        switch field["kind"] as? String {
        case "text":
            try PluginViewValue.keys(field, common.union(["required", "maxLength"]))
            guard let required = field["required"] as? Bool,
                  let number = field["required"] as? NSNumber,
                  CFGetTypeID(number) == CFBooleanGetTypeID() else { throw Failure.invalidResponse }
            kind = .text(required: required, maxLength: try PluginViewValue.integer(field["maxLength"], range: 1...256))
        case "toggle":
            try PluginViewValue.keys(field, common)
            kind = .toggle
        case "choice":
            try PluginViewValue.keys(field, common.union(["options"]))
            guard let raw = field["options"] as? [[String: Any]], (1...8).contains(raw.count) else { throw Failure.invalidResponse }
            let options = try raw.map { option -> Field.Option in
                try PluginViewValue.keys(option, ["id", "label"])
                return .init(id: try key(option["id"]), label: try label(option["label"]))
            }
            guard Set(options.map(\.id)).count == options.count else { throw Failure.invalidResponse }
            kind = .choice(options)
        default:
            throw Failure.invalidResponse
        }
        return .init(id: try key(field["id"]), label: try label(field["label"]), kind: kind)
    }

    private static func checkedText(_ value: Any?, maximum: Int, required: Bool) throws -> String {
        let text = try PluginViewValue.text(value, maximum: maximum)
        guard text.unicodeScalars.allSatisfy({ $0.value >= 32 && $0.value != 127 }) else { throw Failure.invalidResponse }
        // Use the Portable SDK's ECMAScript trim set after rejecting controls.
        if required && text.unicodeScalars.allSatisfy({ scalar in
            switch scalar.value {
            case 0x20, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF: return true
            default: return false
            }
        }) { throw Failure.invalidResponse }
        return text
    }

    private static func label(_ value: Any?) throws -> String {
        try checkedText(value, maximum: 96, required: true)
    }

    private static func key(_ value: Any?) throws -> String {
        guard let id = value as? String, id != "constructor", id != "prototype",
              id.range(of: #"\A[A-Za-z][A-Za-z0-9_-]{0,31}\z"#, options: .regularExpression) != nil else {
            throw Failure.invalidResponse
        }
        return id
    }
}

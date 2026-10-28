import Foundation

func connectionErrorMessage(_ error: Swift.Error) -> String {
    if let localized = error as? LocalizedError, let message = localized.errorDescription { return message }
    return String(describing: error)
}

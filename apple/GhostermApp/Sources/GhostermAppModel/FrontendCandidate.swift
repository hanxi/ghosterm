import Foundation
import GhostermCore

/// The source terminal is known; the execution target described by its output
/// is unverified. This record never selects a route or grants a capability.
public struct FrontendCandidate: Identifiable, Sendable {
    public let id: UUID
    public let sourceID, sourceGeneration: UUID
    public let sourceDescription: String
    public let offer: ExtensionDiscovery.Offer
    public let receivedAt: TimeInterval
    public var expired: Bool { ProcessInfo.processInfo.systemUptime - receivedAt >= 30 }
}

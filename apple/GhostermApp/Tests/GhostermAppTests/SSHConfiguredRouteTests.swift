import XCTest
import GhostermCore
@testable import GhostermAppModel

final class SSHConfiguredRouteTests: XCTestCase {
    func testProxyJumpConfigurationAndOverrides() throws {
        let config = try SSHConfig(contents: "Host dest\n HostName target.invalid\n User target-user\n IdentityFile target-key\n ProxyJump via-user@jump:2222\nHost jump\n HostName gateway.invalid\n User config-user\n Port 2200\n IdentityFile jump-key\n")
        let route = try SSHConfiguredRoute.resolve(config: config, aliases: ["dest"], knownHosts: "fixture-known")
        XCTAssertEqual(route.map(\.host), ["gateway.invalid", "target.invalid"])
        XCTAssertEqual(route.map(\.user), ["via-user", "target-user"])
        XCTAssertEqual(route.map(\.port), [2222, 22])
        XCTAssertTrue(route[0].identityPaths.contains("jump-key") && !route[0].identityPaths.contains("target-key"))
    }
    func testProxyJumpNestedFirstHopAndExplicitOrder() throws {
        let config = try SSHConfig(contents: "Host dest\n ProxyJump first,second\nHost first\n ProxyJump outer\nHost second\n ProxyJump ignored\n")
        let route = try SSHConfiguredRoute.resolve(config: config, aliases: ["dest"], knownHosts: nil)
        XCTAssertEqual(route.map(\.alias), ["outer", "first", "second", "dest"])
    }
    func testProxyJumpInvalidSyntaxNeverBecomesDirect() throws {
        for value in ["", "jump extra", "jump,", ",jump", "jump,,other", "@jump", "jump:0", "jump:65536", "jump:x", "ssh://jump/path", "[not-ip]", "::1", "%h", "user@"] {
            let config = try SSHConfig(contents: "Host dest\n ProxyJump " + value + "\n")
            XCTAssertThrowsError(try SSHConfiguredRoute.resolve(config: config, aliases: ["dest"], knownHosts: nil), value)
        }
    }
    func testProxyJumpCyclesAndLimits() throws {
        for text in ["Host a\n ProxyJump a\n", "Host a\n ProxyJump b\nHost b\n ProxyJump a\n", "Host a\n ProxyJump b,c,d,e,f,g,h,i\n"] {
            let config = try SSHConfig(contents: text)
            XCTAssertThrowsError(try SSHConfiguredRoute.resolve(config: config, aliases: ["a"], knownHosts: nil))
        }
        let valid = try SSHConfig(contents: "Host a\n ProxyJump b,c,d,e,f,g,h\n")
        XCTAssertEqual(try SSHConfiguredRoute.resolve(config: valid, aliases: ["a"], knownHosts: nil).count, 8)
    }
    func testProxyJumpNoneAndWildcardLookup() throws {
        let config = try SSHConfig(contents: "Host dest\n ProxyJump unlisted\nHost un*\n User gateway-user\n Port 2222\n ProxyJump none\n")
        let route = try SSHConfiguredRoute.resolve(config: config, aliases: ["dest"], knownHosts: nil)
        XCTAssertEqual(route.map(\.alias), ["unlisted", "dest"])
        XCTAssertEqual(route[0].user, "gateway-user"); XCTAssertEqual(route[0].port, 2222)
    }
    func testProxyJumpURIAndIPv6() throws {
        let jumps = try SSHConfiguredRoute.parse("ssh://alice@[::1]:2222,bob@jump:2200")
        XCTAssertEqual(jumps.map(\.alias), ["::1", "jump"])
        XCTAssertEqual(jumps.map(\.user), ["alice", "bob"])
        XCTAssertEqual(jumps.map(\.port), [2222, 2200])
        XCTAssertEqual(try SSHConfiguredRoute.parse("none,jump").map(\.alias), ["none", "jump"])
    }
    func testProxyJumpUnsupportedHopAndManualAmbiguity() throws {
        for extra in [" ProxyCommand false\n", " Match all\n", " UserKnownHostsFile other\n"] {
            let config = try SSHConfig(contents: "Host dest\n ProxyJump jump\nHost jump\n" + extra)
            XCTAssertThrowsError(try SSHConfiguredRoute.resolve(config: config, aliases: ["dest"], knownHosts: nil))
        }
        let config = try SSHConfig(contents: "Host dest\n ProxyJump jump\n")
        XCTAssertThrowsError(try SSHConfiguredRoute.resolve(config: config, aliases: ["jump", "dest"], knownHosts: nil))
    }
}

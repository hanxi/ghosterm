// macOS asynchronous DNS. No getaddrinfo, blocking worker, or UI-thread lookup.
import Darwin
import Dispatch
import dnssd
import Foundation

public final class SessionAddressResolver {
    public enum Failure: Error, Equatable { case invalidHost, timeout, unavailable(Int32) }
    private let owner: DispatchQueue
    private let host: String
    private var family: UInt8
    private var triedOther = false
    private var reference: DNSServiceRef?
    private var completion: ((Result<RecoverableSession.Address, Failure>) -> Void)?
    private var deadline: DispatchWorkItem?
    private var keepAlive: SessionAddressResolver?
    private var finished = false
    private init(host: String, preferIPv6: Bool, owner: DispatchQueue,
                 completion: @escaping (Result<RecoverableSession.Address, Failure>) -> Void) {
        self.host = host; family = preferIPv6 ? 6 : 4; self.owner = owner; self.completion = completion
    }
    /// Each connection attempt can alternate preferred family; a missing record
    /// falls back to the other family within the same deadline. Completion is
    /// always asynchronous on owner. cancel revokes completion immediately.
    public static func resolve(host: String, preferIPv6: Bool = false, timeoutMilliseconds: UInt32 = 60000,
                               owner: DispatchQueue, completion: @escaping (Result<RecoverableSession.Address, Failure>) -> Void) -> SessionAddressResolver {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        let resolver = SessionAddressResolver(host: host, preferIPv6: preferIPv6, owner: owner, completion: completion)
        resolver.keepAlive = resolver
        owner.async { resolver.start(timeoutMilliseconds) }
        return resolver
    }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    private func start(_ timeout: UInt32) {
        checkOwner(); guard !finished else { return }
        guard timeout > 0, timeout <= 60000, !host.isEmpty, host.utf8.count <= 1024,
              !host.utf8.contains(where: { $0 <= 32 || $0 == 127 }) else { finish(.failure(.invalidHost)); return }
        let work = DispatchWorkItem { [weak self] in self?.finish(.failure(.timeout)) }
        deadline = work; owner.asyncAfter(deadline: .now() + .milliseconds(Int(timeout)), execute: work)
        var ipv4 = in_addr()
        if host.withCString({ inet_pton(AF_INET, $0, &ipv4) }) == 1 {
            let bytes = withUnsafeBytes(of: ipv4) { Array($0) } + Array(repeating: UInt8(0), count: 12)
            finish(.success(.init(family: 4, bytes: bytes))); return
        }
        let parts = host.split(separator: "%", omittingEmptySubsequences: false)
        var ipv6 = in6_addr()
        if parts.count <= 2, let literal = parts.first, String(literal).withCString({ inet_pton(AF_INET6, $0, &ipv6) }) == 1 {
            var scope: UInt32 = 0
            if parts.count == 2 {
                scope = UInt32(parts[1]) ?? String(parts[1]).withCString { if_nametoindex($0) }
                guard scope != 0 else { finish(.failure(.invalidHost)); return }
            }
            finish(.success(.init(family: 6, bytes: withUnsafeBytes(of: ipv6) { Array($0) }, scope: scope))); return
        }
        guard !host.contains(":"), !host.contains("%") else { finish(.failure(.invalidHost)); return }
        query()
    }
    private func query() {
        guard !finished else { return }
        let protocolMask = family == 4 ? DNSServiceProtocol(kDNSServiceProtocol_IPv4) : DNSServiceProtocol(kDNSServiceProtocol_IPv6)
        let error = host.withCString { name in
            DNSServiceGetAddrInfo(&reference, 0, 0, protocolMask, name, { ref, flags, _, error, _, address, _, context in
                guard let context else { return }
                let resolver = Unmanaged<SessionAddressResolver>.fromOpaque(context).takeUnretainedValue()
                resolver.answer(ref, flags, error, address)
            }, Unmanaged.passUnretained(self).toOpaque())
        }
        guard error == kDNSServiceErr_NoError, let reference else { finish(.failure(.unavailable(error == kDNSServiceErr_NoError ? Int32(kDNSServiceErr_Unknown) : error))); return }
        let scheduled = DNSServiceSetDispatchQueue(reference, owner)
        if scheduled != kDNSServiceErr_NoError { finish(.failure(.unavailable(scheduled))) }
    }
    private func answer(_ ref: DNSServiceRef?, _ flags: DNSServiceFlags, _ error: DNSServiceErrorType, _ address: UnsafePointer<sockaddr>?) {
        checkOwner(); guard !finished, ref == reference else { return }
        if error != kDNSServiceErr_NoError {
            if error == kDNSServiceErr_NoSuchRecord && !triedOther {
                stopQuery(); triedOther = true; family = family == 4 ? 6 : 4; query()
            } else { finish(.failure(.unavailable(error))) }
            return
        }
        guard flags & DNSServiceFlags(kDNSServiceFlagsAdd) != 0, let address else { return }
        if Int32(address.pointee.sa_family) == AF_INET, Int(address.pointee.sa_len) >= MemoryLayout<sockaddr_in>.size {
            let a = address.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
            finish(.success(.init(family: 4, bytes: withUnsafeBytes(of: a.sin_addr) { Array($0) } + Array(repeating: 0, count: 12))))
        } else if Int32(address.pointee.sa_family) == AF_INET6, Int(address.pointee.sa_len) >= MemoryLayout<sockaddr_in6>.size {
            let a = address.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { $0.pointee }
            finish(.success(.init(family: 6, bytes: withUnsafeBytes(of: a.sin6_addr) { Array($0) }, scope: a.sin6_scope_id)))
        } else { finish(.failure(.unavailable(Int32(kDNSServiceErr_Unsupported)))) }
    }
    private func stopQuery() { if let reference { DNSServiceRefDeallocate(reference) }; reference = nil }
    private func finish(_ result: Result<RecoverableSession.Address, Failure>?) {
        checkOwner(); guard !finished else { return }; finished = true
        stopQuery(); deadline?.cancel(); deadline = nil
        let notify = completion; completion = nil; keepAlive = nil
        if let result { notify?(result) }
    }
    public func cancel() { checkOwner(); finish(nil) }
    deinit { assert(reference == nil) }
}

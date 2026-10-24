// KeychainStoreTests — 平台安全存储的真实行为断言（真 Keychain）。
//
// 隔离纪律（铁律 #5：密钥材料不进测试快照）：
//   - 每条用例用**一次性随机 service**（非生产 service），测完删净；
//   - 断言的是 API 行为（存/取/删/不存在），材料本体不落任何日志。
//
// 跑在无签名 SPM 二进制里也可用（本机已验证：SecItemAdd/Copy/Delete
// 在 login keychain 上工作正常）。
import GhostermAppModel
import XCTest

final class KeychainStoreTests: XCTestCase {
    /// 生产 service 之外的隔离入口：直接操作底层 set/get/delete，
    /// account 前缀随机，避免污染任何真实记录。
    private func withIsolatedAccount<T>(_ body: (String) throws -> T) rethrows -> T {
        let account = "test-\(UUID().uuidString)"
        defer { KeychainStoreTestHooks.delete(account) }
        return try body(account)
    }

    func testRoundTripPEM() throws {
        try withIsolatedAccount { account in
            let pem = "-----BEGIN OPENSSH PRIVATE KEY-----\nfake\n-----END OPENSSH PRIVATE KEY-----\n"
            try KeychainStoreTestHooks.set(Data(pem.utf8), account: account)
            let back = try KeychainStoreTestHooks.get(account: account)
            XCTAssertEqual(String(decoding: back, as: UTF8.self), pem,
                           "Keychain 往返必须逐字节一致")
        }
    }

    func testGetMissingThrowsNotFound() throws {
        try withIsolatedAccount { account in
            XCTAssertThrowsError(try KeychainStoreTestHooks.get(account: account)) { error in
                guard case KeychainStoreError.notFound = error else {
                    return XCTFail("缺失记录应抛 notFound，得到 \(error)")
                }
            }
        }
    }

    func testOverwriteReplacesValue() throws {
        try withIsolatedAccount { account in
            try KeychainStoreTestHooks.set(Data("first".utf8), account: account)
            try KeychainStoreTestHooks.set(Data("second".utf8), account: account)
            let back = try KeychainStoreTestHooks.get(account: account)
            XCTAssertEqual(String(decoding: back, as: UTF8.self), "second",
                           "重复 set 应覆盖，不得报 errSecDuplicateItem")
        }
    }

    func testDeleteIsIdempotent() throws {
        try withIsolatedAccount { account in
            try KeychainStoreTestHooks.set(Data("x".utf8), account: account)
            KeychainStoreTestHooks.delete(account)
            KeychainStoreTestHooks.delete(account) // 再删一次不炸
            XCTAssertThrowsError(try KeychainStoreTestHooks.get(account: account))
        }
    }

    /// 语义化入口的 account 组装：22 端口裸 host、非 22 加端口、路径前缀。
    func testAccountConventions() {
        XCTAssertEqual(KeychainStore.hostAccount("web.example", port: 22),
                       "pass:web.example")
        XCTAssertEqual(KeychainStore.hostAccount("web.example", port: 2222),
                       "pass:web.example:2222")
        XCTAssertEqual(KeychainStore.keyAccount("/Users/me/.ssh/id_ed25519"),
                       "key:/Users/me/.ssh/id_ed25519")
    }
}

package com.hanxi.ghosterm.bridge

import android.os.Looper

/** Native consent/lifecycle worker, never an App view or JS permission bypass.
 * Only stop is cross-thread. Explicit close retains draining/error ownership. */
class ExtensionSession private constructor(private val worker: Thread) : AutoCloseable {
    enum class Role { HOST, EXECUTOR }
    enum class PeerKind { LOCAL, SSH }
    enum class Decision { UNKNOWN, ALLOW, DENY }
    enum class Access { UNKNOWN, ALLOWED, DENIED, REVOKED, UNSUPPORTED }
    enum class Phase { SENDING, WAIT_HELLO, WAIT_OFFER, CONSENT, WAIT_DECISION, WAIT_ACCEPT,
        WAIT_READY, WAIT_ACK, ACTIVE, ENDING, ENDED, CLOSED }
    enum class End { NONE, DENIED, REVOKED, EXPIRED, INCOMPATIBLE, PROTOCOL, LOST, PEER, LOCAL, INTERNAL }
    data class Budgets(val fragmentBytes: Int = 768, val messageBytes: Int = 65536, val resourceBytes: Int = 262144)
    data class Config(val pluginId: String, val pluginVersion: String, val clientDigest: String, val executorDigest: String,
        val hostVersion: String, val sdkVersion: String = "1.0.0", val required: List<String>, val available: List<String>,
        val peerKind: PeerKind, val peerIdentity: String, val budgets: Budgets = Budgets(),
        val handshakeMilliseconds: Int = 5000, val consentMilliseconds: Int = 60000)
    data class Info(val phase: Phase, val end: End, val error: Int, val cleanupError: Int, val decision: Decision,
        val connection: String, val instance: String, val consentToken: String, val peerHostVersion: String,
        val epoch: Long, val consentDeadline: Long, val received: Long, val sent: Long,
        val budgets: Budgets, val peerKind: PeerKind, val peerIdentity: String)
    class Lease internal constructor(internal val bytes: ByteArray)
    @Volatile private var handle = 0L
    private fun owner() {
        check(Thread.currentThread() === worker && Looper.myLooper() != Looper.getMainLooper()) {
            "插件会话必须在创建它的非 UI worker 中调用"
        }
        check(handle != 0L) { "插件会话已释放" }
    }
    fun step(milliseconds: Int = 0): Phase { owner(); require(milliseconds >= 0)
        return Phase.entries[NativeExtensionSession.step(handle, milliseconds)] }
    fun info(): Info {
        owner(); val fields = NativeExtensionSession.info(handle); val v = fields[0] as LongArray
        @Suppress("UNCHECKED_CAST") val texts = fields[1] as Array<ByteArray>
        check(v.size == 13 && texts.size == 5)
        val t = texts.map { it.toString(Charsets.UTF_8) }
        return Info(Phase.entries[v[0].toInt()], End.entries[v[1].toInt()], v[2].toInt(), v[12].toInt(), Decision.entries[v[3].toInt()],
            t[0], t[1], t[2], t[3], v[4], v[5], v[6], v[7], Budgets(v[8].toInt(), v[9].toInt(), v[10].toInt()),
            PeerKind.entries[v[11].toInt()], t[4])
    }
    fun decide(ticket: String, decision: Decision): Int {
        owner(); require(!ticket.contains('\u0000')); return NativeExtensionSession.decide(handle, ticket.toByteArray(), decision.ordinal)
    }
    fun lease(): Lease? { owner(); return NativeExtensionSession.lease(handle)?.let { check(it.size == 107); Lease(it) } }
    fun access(lease: Lease, capability: String): Access {
        owner(); require(!capability.contains('\u0000'))
        return Access.entries[NativeExtensionSession.access(handle, lease.bytes, capability.toByteArray())]
    }
    fun revoke() { owner(); NativeExtensionSession.revoke(handle) }
    fun stop() { val id = handle; if (id != 0L) NativeExtensionSession.stop(id) }
    fun closeStep(milliseconds: Int = 0): Boolean {
        owner(); require(milliseconds >= 0)
        return when (NativeExtensionSession.close(handle, milliseconds)) {
            0 -> false
            1 -> { handle = 0L; true }
            else -> throw ExtensionChannelException(3, "会话仍有未释放资源，请由 worker 重试")
        }
    }
    override fun close() { if (handle == 0L) return; owner(); repeat(32) { if (closeStep()) return }
        throw ExtensionChannelException(3, "会话仍在释放，请由 worker 重试") }
    companion object {
        /** Success invalidates channel; failure leaves it usable. Metadata is
         * copied from a trusted package/endpoint snapshot, never peer output. */
        fun adopt(channel: ExtensionChannel, role: Role, config: Config): ExtensionSession {
            val result = ExtensionSession(Thread.currentThread())
            val texts = arrayOf(config.pluginId, config.pluginVersion, config.clientDigest, config.executorDigest,
                config.hostVersion, config.sdkVersion, config.peerIdentity).map { it.toByteArray() }.toTypedArray()
            val numbers = intArrayOf(config.peerKind.ordinal, config.budgets.fragmentBytes, config.budgets.messageBytes,
                config.budgets.resourceBytes, config.handshakeMilliseconds, config.consentMilliseconds, 0)
            result.handle = channel.adoptSession(role.ordinal, texts, config.required.map { it.toByteArray() }.toTypedArray(),
                config.available.map { it.toByteArray() }.toTypedArray(), numbers)
            return result
        }
    }
}
internal object NativeExtensionSession {
    init { System.loadLibrary("ghosterm_android") }
    external fun step(handle: Long, milliseconds: Int): Int
    external fun info(handle: Long): Array<Any>
    external fun decide(handle: Long, ticket: ByteArray, decision: Int): Int
    external fun lease(handle: Long): ByteArray?
    external fun access(handle: Long, lease: ByteArray, capability: ByteArray): Int
    external fun revoke(handle: Long)
    external fun stop(handle: Long)
    external fun close(handle: Long, milliseconds: Int): Int
}

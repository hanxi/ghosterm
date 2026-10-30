package com.hanxi.ghosterm.bridge

/** Immutable resolved candidate; IPv6 scope belongs to the address identity. */
class ResolvedSSHAddress(address: ByteArray, val scope: Int = 0) {
    private val binaryAddress = address.copyOf()
    internal fun bytes(): ByteArray = binaryAddress.copyOf()
}
/** An immutable connection request, with DNS already resolved off the UI thread.
 * host remains the original known_hosts identity; address never replaces it.
 * timeoutMillis is the REMAINING total authentication budget after DNS.
 */
class ResolvedSSHConnection(val host: String, val port: Int, val user: String,
    val knownHosts: String, addresses: List<ResolvedSSHAddress>, val timeoutMillis: Int = 60_000) {
    private val candidates = addresses.toList()
    constructor(host: String, port: Int, user: String, knownHosts: String,
        address: ByteArray, scope: Int = 0, timeoutMillis: Int = 60_000) :
        this(host, port, user, knownHosts, listOf(ResolvedSSHAddress(address, scope)), timeoutMillis)
    internal fun openFiles(): Long = NativeFiles.create(host.toByteArray(Charsets.UTF_8), port,
        user.toByteArray(Charsets.UTF_8), knownHosts.toByteArray(Charsets.UTF_8),
        candidates.map { it.bytes() }.toTypedArray(), candidates.map { it.scope }.toIntArray(), timeoutMillis)
    internal fun open(): Long = NativeSession.createRemote(host.toByteArray(Charsets.UTF_8), port,
        user.toByteArray(Charsets.UTF_8), knownHosts.toByteArray(Charsets.UTF_8),
        candidates.map { it.bytes() }.toTypedArray(), candidates.map { it.scope }.toIntArray(), timeoutMillis)
}

/** SSH authentication and terminal IO share one native owner. No terminal bytes enter Kotlin. */
class RemoteSession(connection: ResolvedSSHConnection) : NativeTerminalSession(connection.open(), true) {
    fun decide(prompt: SSHAuthState, accept: Boolean) = respond(prompt, if (accept) 1 else 2)
    /** Buffers are consumed and zeroed on success AND failure. Never pass an immutable password String. */
    fun password(prompt: SSHAuthState, password: ByteArray) = respond(prompt, 3, password)
    fun privateKey(prompt: SSHAuthState, pem: ByteArray, passphrase: ByteArray = byteArrayOf()) = respond(prompt, 4, pem, passphrase)
}
enum class SSHAuthPhase { TCP, NEGOTIATING, HOSTKEY, AUTHENTICATING, READY, TAKEN, FAILED, CANCELLED, CREDENTIALS }
enum class SSHHostState { UNCHECKED, KNOWN, UNKNOWN, MISMATCH, REVOKED }
class SSHAuthState internal constructor(internal val owner: Long, val request: Long, val phase: SSHAuthPhase,
    val error: Int, val host: SSHHostState, val deadline: Boolean, val methods: Int,
    val keyType: String, val fingerprint: String, internal val scope: Any? = null) {
    val description: String get() = when (phase) {
        SSHAuthPhase.TCP -> "正在连接 SSH…"
        SSHAuthPhase.NEGOTIATING -> "正在协商 SSH…"
        SSHAuthPhase.HOSTKEY -> "等待确认主机密钥"
        SSHAuthPhase.CREDENTIALS -> if (error == 4) "认证失败，请重新提供凭据" else "等待 SSH 凭据"
        SSHAuthPhase.AUTHENTICATING -> "正在认证…"
        SSHAuthPhase.READY, SSHAuthPhase.TAKEN -> "正在打开远端终端…"
        SSHAuthPhase.CANCELLED -> "连接已取消"
        SSHAuthPhase.FAILED -> when {
            deadline -> "SSH 连接超时"
            host == SSHHostState.REVOKED -> "主机密钥已撤销，连接已停止"
            host == SSHHostState.MISMATCH -> "主机密钥已变化，连接已停止"
            error == 3 && host == SSHHostState.UNKNOWN -> "无法保存主机信任记录，连接已停止"
            error == 5 -> "主机验证失败或已拒绝，连接已停止"
            error == 7 -> "此 SSH 配置或认证方式暂不支持"
            error == 4 -> "SSH 认证失败"
            else -> "SSH 连接失败（错误 $error）"
        }
    }
    internal companion object {
        fun decode(owner: Long, fields: Array<ByteArray>, scope: Any? = null): SSHAuthState {
            check(fields.size == 8) { "SSH 状态格式无效" }
            val f = fields.map { it.toString(Charsets.UTF_8) }
            return SSHAuthState(owner, f[0].toLong(), SSHAuthPhase.entries[f[1].toInt()], f[2].toInt(),
                SSHHostState.entries[f[3].toInt()], f[4] == "1", f[5].toInt(), f[6], f[7], scope)
        }
    }
}

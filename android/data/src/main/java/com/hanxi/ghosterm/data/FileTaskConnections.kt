package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.bridge.RemoteFileException
import com.hanxi.ghosterm.bridge.RemoteFileSession
import com.hanxi.ghosterm.bridge.ResolvedSSHConnection
import com.hanxi.ghosterm.domain.FileEndpoint
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicBoolean

/** One copy attempt owns separate source/destination connections, including when
 * both endpoints name the same server. Resolver/authentication callbacks consume
 * the frozen endpoint; they cannot supply an existing browser/terminal session.
 * No implicit reconnect or retry is performed after a possible side effect. */
class FileTaskConnections(
    source: FileEndpoint,
    destination: FileEndpoint,
    private val resolve: suspend (Role, FileEndpoint) -> ResolvedSSHConnection,
    private val authenticate: suspend (Role, FileEndpoint, RemoteFileSession) -> Unit,
) {
    enum class Role { SOURCE, DESTINATION }
    private val endpoints = mapOf(Role.SOURCE to source, Role.DESTINATION to destination)
    private val started = AtomicBoolean()

    init {
        endpoints.values.forEach { endpoint ->
            endpoint.remote?.let { identity ->
                require(endpoint.id.isNotEmpty() && endpoint.treeUri == null &&
                    identity.host.isNotBlank() && identity.user.isNotBlank() && identity.port in 1..65535) {
                    "任务远端身份无效"
                }
            }
        }
    }

    class Scope internal constructor(
        private val endpoints: Map<Role, FileEndpoint>,
        private val bindings: Map<Role, RemoteFileBinding>,
    ) {
        private val active = AtomicBoolean(true)
        /** A local endpoint needs no SSH owner. Full endpoint equality prevents
         * accidentally using a refreshed browser identity or the other role. */
        fun remote(role: Role, endpoint: FileEndpoint): RemoteFileBinding? {
            check(active.get()) { "任务连接作用域已结束" }
            require(endpoints[role] == endpoint) { "请求不属于任务的固定端点" }
            return bindings[role]?.also {
                check(it.session.state().ready) { "任务文件连接已失效，请重新确认后重试" }
            }
        }
        internal fun invalidate() { active.set(false) }
    }

    /** Children of block finish before cleanup. Even cancellation/failure while
     * acquiring the second connection closes and awaits every acquired owner. */
    suspend fun <T> use(block: suspend CoroutineScope.(Scope) -> T): T {
        check(started.compareAndSet(false, true)) { "任务连接只能使用一次" }
        val owned = mutableListOf<RemoteFileSession>()
        val bindings = linkedMapOf<Role, RemoteFileBinding>()
        var scope: Scope? = null
        var primary: Throwable? = null
        try {
            return coroutineScope {
                for((role, endpoint) in endpoints) {
                    ensureActive()
                    val identity = endpoint.remote ?: continue
                    val connection = resolve(role, endpoint)
                    ensureActive() // A resolver may return after cancellation.
                    require(connection.host == identity.host && connection.port == identity.port &&
                        connection.user == identity.user) { "解析结果与任务的固定身份不一致" }
                    val session = RemoteFileSession(connection)
                    owned.add(session) // No suspension before taking ownership.
                    authenticate(role, endpoint, session)
                    currentCoroutineContext().ensureActive()
                    while(true) {
                        val state = session.state()
                        if(state.ready) break
                        if(state.finished) throw RemoteFileException(state.error)
                        delay(10)
                    }
                    bindings[role] = RemoteFileBinding(endpoint, session)
                }
                val ready = Scope(endpoints, bindings.toMap())
                scope = ready
                block(ready)
            }
        } catch(failure: Throwable) {
            primary = failure
            throw failure
        } finally {
            scope?.invalidate()
            withContext(NonCancellable) {
                var cleanup: Throwable? = null
                fun record(failure: Throwable) {
                    if(cleanup == null) cleanup = failure else cleanup!!.addSuppressed(failure)
                }
                // Signal all first: destination cleanup never waits for a still
                // live source operation. Every completion is awaited separately.
                owned.forEach { try { it.close() } catch(failure: Throwable) { record(failure) } }
                owned.forEach { try { it.closeAndAwait() } catch(failure: Throwable) { record(failure) } }
                cleanup?.let { if(primary != null) primary!!.addSuppressed(it) else throw it }
            }
        }
    }
}

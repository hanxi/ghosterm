package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.util.UUID

/** Immutable Native launch selection. Peer frames cannot register instances. */
internal data class ExtensionGroupInvocation(val alias: String,
    val launch: ExtensionPackagesViewModel.PreparedLaunch, val temporaryDirectory: String,
    val id: UUID = UUID.randomUUID())

internal class ExtensionConnectionModel(private val application: Application,
    val controller: ExtensionController, private val scope: CoroutineScope) {
    val id: UUID = UUID.randomUUID()
    internal class Instance(val id: UUID, val title: String, val model: ExtensionPackagesViewModel,
        val store: ViewModelStore) {
        var closed by mutableStateOf(false)
            internal set
    }
    val instances = mutableStateListOf<Instance>()
    var title by mutableStateOf("")
        private set
    var busy by mutableStateOf(false)
        private set
    var closed by mutableStateOf(false)
        private set
    var cleanupPending by mutableStateOf(false)
        private set
    var issue: String? by mutableStateOf(null)
        private set
    var state: ExtensionController.MultiplexState? by mutableStateOf(null)
        private set
    private var launchJob: Job? = null
    private var monitor: Job? = null
    private var closing = false
    private var attempt: SSHAttempt? = null
    private var sessions: SessionsViewModel? = null
    private val cleanup = Mutex()

    suspend fun start(launches: List<ExtensionGroupInvocation>, target: PreparedSSH, sessions: SessionsViewModel) {
        check(launchJob == null && !closed && !closing)
        launchJob = currentCoroutineContext()[Job]; busy = true; this.sessions = sessions
        try {
            val first = launches.first()
            require(launches.size in 1..8 && launches.all { it.alias == first.alias && it.alias == target.alias &&
                it.temporaryDirectory == first.temporaryDirectory && it.launch.request.executable == first.launch.request.executable }) {
                "同一启动组必须使用同一目标、gterm 与明确临时目录"
            }
            title = target.alias
            for (launch in launches) {
                val selected = launch.launch
                val exact = controller.resolve(selected.command.path, selected.entry.review.id, selected.entry.review.digest)
                check(exact.status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK && exact.command == selected.command) { "启动组的批准来源或命令已变化" }
                controller.decodeClientArguments(selected.entry.review.id, selected.entry.review.digest, selected.command.path, selected.request.arguments)
            }
            val scopes = launches.mapIndexed { index, launch -> ExtensionController.MultiplexScope(hex(launch.id), (index + 1).toULong()) }
            val command = ExtensionRelayBootstrap.buildMultiplex(hex(id), scopes, launches.map { it.launch.request }, first.temporaryDirectory)
            val attemptID = checkNotNull(sessions.connectExtensionBootstrap(target, command, controller) { ready ->
                check(!closing && ready.info.phase == 1) { "共享连接启动已取消或通道尚未就绪" }
                val children = controller.adoptMultiplexConnection(hex(id), scopes)
                children.zip(launches).forEach { (owner, invocation) ->
                    check(!closing)
                    val store = ViewModelStore()
                    val factory = object : ViewModelProvider.Factory {
                        @Suppress("UNCHECKED_CAST")
                        override fun <T : ViewModel> create(modelClass: Class<T>): T {
                            check(modelClass == ExtensionPackagesViewModel::class.java)
                            return ExtensionPackagesViewModel(application, owner) as T
                        }
                    }
                    val model = ViewModelProvider(store, factory)[ExtensionPackagesViewModel::class.java]
                    instances.add(Instance(invocation.id, invocation.launch.command.title, model, store))
                    model.startMultiplexInstance(invocation.launch, target.alias, command)
                }
                true
            }) { "已有 SSH 认证请求或此构建未启用插件" }
            attempt = sessions.connecting?.takeIf { it.id == attemptID }
            while (sessions.connecting?.id == attemptID) {
                currentCoroutineContext().ensureActive()
                check(sessions.connecting?.failed != true) { sessions.connecting?.message ?: "连接失败" }
                delay(25)
            }
            currentCoroutineContext().ensureActive()
            check(instances.size == launches.size && !closing) { "共享连接启动未确认" }
            monitor = scope.launch {
                try {
                    while (!closed) {
                        state = controller.stepMultiplexConnection()
                        if (state?.info?.error != 0) issue = "共享连接结束或发生错误，请检查各实例结果"
                        if (instances.all { it.closed }) { close(); break }
                        delay(100)
                    }
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (failure: Exception) { issue = "共享连接状态查不出来：${failure.message}" }
            }
        } catch (failure: Exception) {
            if (failure !is CancellationException) issue = "连接组启动失败：${failure.message ?: failure.javaClass.simpleName}"
            withContext(NonCancellable) { release() }
            if (failure is CancellationException) throw failure
        } finally { launchJob = null; busy = false }
    }

    /** The root ViewModel cancels every child UI scope; SessionsViewModel owns
     * the shared Native library's final close even if its screen is gone. */
    fun dispose() {
        closing = true; launchJob?.cancel(); monitor?.cancel()
        instances.forEach { it.store.clear() }
    }
    fun requestClose() { scope.launch { close() } }
    fun requestCloseInstance(instance: Instance) { scope.launch {
        try { closeInstance(instance) }
        catch (failure: Exception) { issue = "实例释放失败：${failure.message}"; cleanupPending = true }
    } }
    private suspend fun closeInstance(instance: Instance) {
        if (instance.closed) return
        instance.store.clear() // Cancels its UI/RPC pumps before Native owner teardown.
        instance.model.closeInstance()
        instance.closed = true
    }
    suspend fun close(): Boolean {
        closing = true
        val job = launchJob
        if (job != null && job !== currentCoroutineContext()[Job]) { job.cancel(); job.join() }
        return withContext(NonCancellable) { release() }
    }
    private suspend fun release(): Boolean = cleanup.withLock {
        if (closed) return@withLock true
        try {
            attempt?.let { current ->
                sessions?.let { owner -> if (owner.connecting === current) owner.cancelConnection(current.id) }
                current.job?.join()
            }
            instances.forEach { closeInstance(it) }
            // Retain owners and expose retry if any logical/physical close still drains.
            controller.close()
            state = controller.multiplexObservation()
            if (state?.transport?.remoteEof == false) issue = "连接已释放；执行端退出未确认，请检查各实例回执"
            cleanupPending = false; closed = true
            monitor?.cancel(); monitor = null
            true
        } catch (failure: Exception) {
            cleanupPending = true
            issue = "连接或资源仍待释放：${failure.message ?: failure.javaClass.simpleName}"
            false
        }
    }
    private fun hex(value: UUID) = value.toString().replace("-", "")
}

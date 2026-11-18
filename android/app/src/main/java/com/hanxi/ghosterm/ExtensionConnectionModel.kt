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
import org.json.JSONObject
import android.os.SystemClock

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
        var closeRequested by mutableStateOf(false)
            internal set
        internal val cleanup = Mutex()
        val closeNeedsConfirmation get() = !closed && !closeRequested
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
    private var closing by mutableStateOf(false)
    val closeNeedsConfirmation get() = !closed && !closing &&
        (busy || instances.any { it.closeNeedsConfirmation })
    private var nativeReady = false
    private var origin: ExtensionGroupInvocation? = null
    private var nextEpoch = 1uL
    private val scopes = mutableMapOf<UUID, ExtensionController.MultiplexScope>()
    var targetDescription by mutableStateOf("")
        private set
    val canAdd get() = nativeReady && !closed && !closing && !busy && !cleanupPending &&
        (state?.info?.error ?: 0) == 0 && instances.count { !it.closed } < 8
    private var attempt: SSHAttempt? = null
    private var sessions: SessionsViewModel? = null
    private val cleanup = Mutex()

    suspend fun start(launches: List<ExtensionGroupInvocation>, target: PreparedSSH, sessions: SessionsViewModel): Boolean {
        check(launchJob == null && !closed && !closing)
        launchJob = currentCoroutineContext()[Job]; busy = true; this.sessions = sessions
        try {
            val first = launches.first()
            require(launches.size in 1..8 && launches.all { it.alias == first.alias && it.alias == target.alias &&
                it.temporaryDirectory == first.temporaryDirectory && it.launch.request.executable == first.launch.request.executable }) {
                "同一启动组必须使用同一目标、gterm 与明确临时目录"
            }
            title = target.alias; origin = first; nextEpoch = (launches.size + 1).toULong()
            for (launch in launches) {
                val selected = launch.launch
                val exact = controller.resolve(selected.command.path, selected.entry.review.id, selected.entry.review.digest)
                check(exact.status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK && exact.command == selected.command) { "启动组的批准来源或命令已变化" }
                controller.decodeClientArguments(selected.entry.review.id, selected.entry.review.digest, selected.command.path, selected.request.arguments)
            }
            val scopes = launches.mapIndexed { index, launch -> ExtensionController.MultiplexScope(hex(launch.id), (index + 1).toULong()) }
            val command = ExtensionRelayBootstrap.buildMultiplex(hex(id), scopes, launches.map { it.launch.request }, first.temporaryDirectory, live = true)
            val attemptID = checkNotNull(sessions.connectExtensionBootstrap(target, command, controller) { ready ->
                check(!closing && ready.info.phase == 1) { "共享连接启动已取消或通道尚未就绪" }
                val children = controller.adoptMultiplexConnection(hex(id), scopes, live = true)
                targetDescription = controller.multiplexTargetDescription()
                val hello = JSONObject(controller.manageMultiplex("hello"))
                check(hello.opt("nativeControl") is Int && hello.getInt("nativeControl") == 1) { "执行端未确认在线管理能力" }
                nativeReady = true
                children.zip(launches).forEach { (owner, invocation) ->
                    check(!closing)
                    val model = createInstance(owner, invocation)
                    this@ExtensionConnectionModel.scopes[invocation.id] = scopes[launches.indexOf(invocation)]
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
                        delay(100)
                    }
                } catch (cancelled: CancellationException) { throw cancelled }
                catch (failure: Exception) { issue = "共享连接状态查不出来：${failure.message}" }
            }
            return true
        } catch (failure: Exception) {
            if (failure !is CancellationException) issue = "连接组启动失败：${failure.message ?: failure.javaClass.simpleName}"
            withContext(NonCancellable) { controller.stopMultiplexConnection(); release() }
            if (failure is CancellationException) throw failure
            return false
        } finally { launchJob = null; busy = false }
    }

    private fun createInstance(owner: ExtensionController, invocation: ExtensionGroupInvocation): ExtensionPackagesViewModel {
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
        return model
    }
    fun acceptsConfiguration(invocation: ExtensionGroupInvocation): Boolean {
        val first = origin ?: return false
        val request = invocation.launch.request
        return invocation.alias == first.alias && invocation.temporaryDirectory == first.temporaryDirectory &&
            request.executable == first.launch.request.executable && request.attachment == null &&
            (request.execution == null || request.execution.temporaryDirectory == first.temporaryDirectory)
    }
    suspend fun add(invocation: ExtensionGroupInvocation): Instance? {
        if (!canAdd) { issue = "连接正在处理请求、已结束或八个实例名额已满"; return null }
        launchJob = currentCoroutineContext()[Job]; busy = true; issue = null
        var child: ExtensionController? = null
        var added: Instance? = null
        try {
            val first = checkNotNull(origin); val request = invocation.launch.request
            require(acceptsConfiguration(invocation)) {
                "新增实例须沿用此连接的目标、gterm 与临时目录；更换范围请新建连接"
            }
            val exact = controller.resolve(invocation.launch.command.path, invocation.launch.entry.review.id, invocation.launch.entry.review.digest)
            check(exact.status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK && exact.command == invocation.launch.command) { "来源或命令批准已变化" }
            controller.decodeClientArguments(invocation.launch.entry.review.id, invocation.launch.entry.review.digest, invocation.launch.command.path, request.arguments)
            val profile = ExtensionRelayBootstrap.multiplexProfile(request)
            val deadline = SystemClock.elapsedRealtime() + 3000
            for (instance in instances.filter { it.closed }) {
                scopes[instance.id]?.let { old ->
                    val input = JSONObject().put("instance", old.instance).put("epoch", old.epoch.toString()).toString()
                    while (JSONObject(controller.manageMultiplex("status", input)).opt("present") != false) {
                        check(SystemClock.elapsedRealtime() < deadline) { "上一实例仍在清理，请稍后重试" }; delay(10)
                    }
                }
                scopes.remove(instance.id)
            }
            instances.removeAll { it.closed }
            check(instances.size < 8 && nextEpoch != ULong.MAX_VALUE)
            val scope = ExtensionController.MultiplexScope(hex(invocation.id), nextEpoch++)
            val owner = withContext(NonCancellable) { controller.openMultiplexInstance(scope) }; child = owner
            val input = JSONObject().put("instance", scope.instance).put("epoch", scope.epoch.toString()).put("profile", profile).toString()
            check(JSONObject(controller.manageMultiplex("open", input)).opt("admitted") == true) { "新增实例准入未确认" }
            currentCoroutineContext().ensureActive()
            val model = createInstance(owner, invocation); scopes[invocation.id] = scope
            val instance = instances.last()
            added = instance
            model.startMultiplexInstance(invocation.launch, first.alias, "此连接的在线实例 · " + targetDescription)
            currentCoroutineContext().ensureActive()
            return instance
        } catch (failure: Exception) {
            issue = "新增实例失败：${failure.message ?: failure.javaClass.simpleName}"
            withContext(NonCancellable) {
                try {
                    val instance = added
                    if (instance != null) closeInstance(instance) else child?.close()
                } catch (_: Exception) {
                    cleanupPending = true
                }
            }
            if (failure is CancellationException) throw failure
            return null
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
        if (instances.none { it === instance }) return@launch
        try { closeInstance(instance) }
        catch (failure: Exception) { issue = "实例释放失败：${failure.message}"; cleanupPending = true }
    } }
    private suspend fun closeInstance(instance: Instance) = instance.cleanup.withLock {
        if (instance.closed) return@withLock
        instance.closeRequested = true
        instance.store.clear() // Cancels its UI/RPC pumps before Native owner teardown.
        instance.model.closeInstance()
        instance.closed = true
    }
    suspend fun close(): Boolean {
        closing = true; monitor?.cancel(); monitor = null
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
            var released = true
            instances.forEach { try { closeInstance(it) } catch (_: Exception) { released = false } }
            if (released && nativeReady) {
                try { check(JSONObject(controller.manageMultiplex("finish")).opt("released") == true) }
                catch (failure: Exception) { issue = "连接关闭结果未确认：${failure.message}"; controller.stopMultiplexConnection() }
            } else controller.stopMultiplexConnection()
            instances.filter { !it.closed }.forEach { closeInstance(it) }
            // Retain owners and expose retry if any logical/physical close still drains.
            controller.close()
            state = controller.multiplexObservation()
            if (state?.transport?.remoteEof == false) issue = "连接已释放；执行端退出未确认，请检查各实例回执"
            cleanupPending = false; closed = true; nativeReady = false
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

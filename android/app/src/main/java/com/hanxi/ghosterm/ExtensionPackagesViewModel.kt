package com.hanxi.ghosterm

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Per-AppModel review state. Trust is deliberately process-local in this slice. */
class ExtensionPackagesViewModel(application: Application) : AndroidViewModel(application) {
    data class Entry(val review: ExtensionController.Review, val decision: String = "尚未批准")
    data class ActiveSession(
        val packageID: String, val packageDigest: String, val command: ExtensionController.Command,
        val arguments: List<String>, val executorDigest: String, val peerIdentity: String, val info: ExtensionSession.Info,
    )

    internal val controller: ExtensionController? = if (BuildConfig.PLUGIN_JS_ENABLED && ExtensionRuntime.supportStatus() == 0) {
        runCatching {
            ExtensionController(
                ExtensionController.HostProfile(
                    sdkVersion = "1.0.0", protocols = listOf(1), runtimes = listOf("portable-js"),
                    capabilities = listOf("session.rpc")),
                application.applicationContext)
        }.getOrNull()
    } else null

    val runtimeAvailable get() = controller != null
    var busy by mutableStateOf(false)
        private set
    var error: String? by mutableStateOf(null)
        private set
    var entries: List<Entry> by mutableStateOf(emptyList())
        private set
    var activeSession: ActiveSession? by mutableStateOf(null)
        private set
    var activeRuntime: ExtensionController.RuntimeState? by mutableStateOf(null)
        private set
    var launchBusy by mutableStateOf(false)
        private set

    fun launch(entry: Entry, command: ExtensionController.Command, target: PreparedSSH,
        request: ExtensionRelayBootstrap.Request, sessions: SessionsViewModel) {
        val owner = controller ?: return
        if (launchBusy) return
        if (sessions.connecting != null) { error = "当前已有 SSH 连接请求，请完成或取消后再启动插件"; return }
        if (entry.decision != "已批准（本进程）" || entry.review.digest != request.clientDigest ||
            entry.review.commands.none { it.path == command.path }) {
            error = "只能启动本进程已批准摘要中的命令"; return
        }
        if (activeSession != null || activeRuntime != null) { error = "此插件控制器已有活动 Session"; return }
        launchBusy = true; error = null
        val id = sessions.connectExtensionRoute(target, request) { ready ->
            try {
                check(ready.info.phase == 1) { "SSH 插件通道尚未就绪" }
                val adopted = owner.adoptSSHSession(entry.review.id, entry.review.digest,
                    request.executorDigest, "0.1.0", target.alias, emptyList())
                activeSession = ActiveSession(entry.review.id, entry.review.digest, command,
                    request.arguments.toList(), request.executorDigest, target.alias, adopted.info)
                launchBusy = false
                viewModelScope.launch { advanceToConsent(owner) }
                true
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "插件 Session 接管失败：${failure.message ?: failure.javaClass.simpleName}"
                launchBusy = false
                false
            }
        }
        if (id == null) {
            launchBusy = false
            error = "无法开始插件 SSH 连接；请确认没有其它连接正在进行"
        } else {
            viewModelScope.launch {
                while (sessions.connecting?.id == id) delay(25)
                if (activeSession == null) launchBusy = false
            }
        }
    }

    fun reportError(message: String) { error = message }

    fun decideConsent(allow: Boolean) {
        val owner = controller ?: return
        val current = activeSession ?: return
        if (launchBusy || current.info.phase != ExtensionSession.Phase.CONSENT || current.info.consentToken.isBlank()) return
        launchBusy = true
        viewModelScope.launch {
            try {
                val decision = if (allow) ExtensionSession.Decision.ALLOW else ExtensionSession.Decision.DENY
                updateSession(owner.decideSession(current.info.consentToken, decision))
                if (allow) {
                    while (true) {
                        val state = owner.stepSession(10)
                        updateSession(state)
                        when (state.info.phase) {
                            ExtensionSession.Phase.ACTIVE -> {
                                val active = checkNotNull(activeSession)
                                activeRuntime = owner.activateClient(active.packageID, active.packageDigest,
                                    active.command.path, active.arguments)
                                launchBusy = false
                                viewModelScope.launch { advanceRuntime(owner) }
                                break
                            }
                            ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED -> break
                            else -> delay(20)
                        }
                    }
                } else {
                    while (activeSession?.info?.phase != ExtensionSession.Phase.ENDED &&
                        activeSession?.info?.phase != ExtensionSession.Phase.CLOSED) {
                        updateSession(owner.stepSession(10)); delay(20)
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "插件授权或启动失败：${failure.message ?: failure.javaClass.simpleName}"
            } finally { launchBusy = false }
        }
    }

    fun stopRuntime() {
        val owner = controller ?: return
        if (launchBusy || activeRuntime == null) return
        launchBusy = true
        owner.requestStopRuntime()
    }

    fun stopSession() {
        val owner = controller ?: return
        val current = activeSession ?: return
        if (launchBusy || current.info.phase != ExtensionSession.Phase.ACTIVE || activeRuntime != null) return
        launchBusy = true
        owner.requestStopSession()
        viewModelScope.launch {
            try {
                while (true) {
                    val state = owner.stepSession(10)
                    updateSession(state)
                    if (state.info.phase in setOf(ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED)) break
                    delay(20)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { error = "停止插件 Session 失败：${failure.message ?: failure.javaClass.simpleName}" }
            finally { launchBusy = false }
        }
    }

    fun releaseFinished() {
        val owner = controller ?: return
        if (launchBusy) return
        launchBusy = true
        viewModelScope.launch {
            try {
                owner.releaseFinished()
                activeRuntime = null; activeSession = null
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { error = "释放插件资源失败：${failure.message ?: failure.javaClass.simpleName}" }
            finally { launchBusy = false }
        }
    }

    private suspend fun advanceToConsent(owner: ExtensionController) {
        try {
            while (true) {
                val state = owner.stepSession(10)
                updateSession(state)
                when (state.info.phase) {
                    ExtensionSession.Phase.CONSENT -> delay(100)
                    ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED,
                    ExtensionSession.Phase.ACTIVE, ExtensionSession.Phase.WAIT_DECISION -> return
                    else -> delay(20)
                }
            }
        } catch (cancelled: CancellationException) { throw cancelled }
        catch (failure: Throwable) { error = "插件 Session 握手失败：${failure.message ?: failure.javaClass.simpleName}" }
    }

    private suspend fun advanceRuntime(owner: ExtensionController) {
        try {
            while (true) {
                val state = owner.stepRuntime()
                activeRuntime = state
                if (state.info.phase !in setOf(ExtensionRuntime.Phase.ACTIVE, ExtensionRuntime.Phase.STARTING,
                        ExtensionRuntime.Phase.CREATED)) return
                delay(50)
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            error = "插件 Runtime 运行失败：${failure.message ?: failure.javaClass.simpleName}"
        } finally {
            launchBusy = false
        }
    }

    private fun updateSession(state: ExtensionController.SessionState) {
        val current = activeSession ?: return
        activeSession = current.copy(info = state.info)
    }

    fun reviewTree(uri: Uri) {
        val owner = controller ?: return
        if (busy) return
        busy = true
        error = null
        viewModelScope.launch {
            try {
                val review = owner.reviewTree(uri)
                if (entries.none { it.review.id == review.id && it.review.digest == review.digest }) {
                    entries = entries + Entry(review)
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "插件包审查失败：${failure.message ?: failure.javaClass.simpleName}"
            } finally {
                busy = false
            }
        }
    }

    fun decide(entry: Entry, approve: Boolean) {
        val owner = controller ?: return
        if (busy) return
        busy = true
        error = null
        viewModelScope.launch {
            try {
                val status = if (approve) owner.approve(entry.review.id, entry.review.digest)
                    else owner.deny(entry.review.id, entry.review.digest)
                val accepted = if (approve) status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK
                    else status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.DENIED
                if (!accepted) error = "来源裁决未完成：$status"
                else entries = entries.map {
                    if (it.review.id == entry.review.id && it.review.digest == entry.review.digest) {
                        it.copy(decision = if (approve) "已批准（本进程）" else "已拒绝")
                    } else it
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "来源裁决失败：${failure.message ?: failure.javaClass.simpleName}"
            } finally {
                busy = false
            }
        }
    }

}

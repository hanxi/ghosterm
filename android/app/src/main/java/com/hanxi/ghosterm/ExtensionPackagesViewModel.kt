package com.hanxi.ghosterm

import android.app.Application
import android.net.Uri
import android.os.SystemClock
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject

/** Per-AppModel review state. Trust is deliberately process-local in this slice. */
class ExtensionPackagesViewModel(application: Application) : AndroidViewModel(application) {
    private val codexExecutorMethods = listOf("codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond")
    private val codexHostMethods = listOf("codex.event")
    private val pendingCodexRPCHandles = mutableSetOf<Long>()
    private val activeCodexRPCHandles = mutableSetOf<Long>()
    data class Entry(val review: ExtensionController.Review, val decision: String = "尚未批准")
    data class ActiveSession(
        val packageID: String, val packageDigest: String, val command: ExtensionController.Command,
        val arguments: List<String>, val decodedArguments: String?, val methods: List<String>, val hostMethods: List<String>, val executorDigest: String,
        val peerIdentity: String, val remoteCommand: String, val requiredCapabilities: List<String>,
        val info: ExtensionSession.Info,
    )

    internal val controller: ExtensionController? = if (BuildConfig.PLUGIN_JS_ENABLED && ExtensionRuntime.supportStatus() == 0) {
        runCatching {
            ExtensionController(
                ExtensionController.HostProfile(
                    sdkVersion = "1.0.0", protocols = listOf(1), runtimes = listOf("portable-js"),
                    capabilities = listOf("session.rpc", "resource.receive", "file.readScoped")),
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
    var receivedResources: List<ExtensionResourceStore.Content> by mutableStateOf(emptyList())
        private set
    internal var codex: CodexHostState by mutableStateOf(CodexHostState())
        private set
    var launchBusy by mutableStateOf(false)
        private set

    fun launch(entry: Entry, command: ExtensionController.Command, target: PreparedSSH,
        request: ExtensionRelayBootstrap.Request, sessions: SessionsViewModel) {
        val owner = controller ?: return
        val launchRequest = if (command.path == "ai codex") request.copy(
            methods = (request.methods + codexExecutorMethods).distinct(),
            hostMethods = (request.hostMethods + codexHostMethods).distinct()) else request
        if (launchBusy) return
        if (sessions.connecting != null) { error = "当前已有 SSH 连接请求，请完成或取消后再启动插件"; return }
        if (entry.decision != "已批准（本进程）" || entry.review.digest != launchRequest.clientDigest ||
            entry.review.commands.none { it.path == command.path }) {
            error = "只能启动本进程已批准摘要中的命令"; return
        }
        if ((launchRequest.methods.isNotEmpty() || launchRequest.hostMethods.isNotEmpty()) &&
            "session.rpc" !in entry.review.requiredCapabilities) {
            error = "此 Client 包未声明 session.rpc，不能协商 RPC 方法"; return
        }
        if (command.path == "ai codex" && (launchRequest.workspaceRootDirectory.isNullOrBlank() ||
                launchRequest.processPrograms.none { it.name == "codex" })) {
            error = "ai codex 需要明确授权工作区，并配置名为 codex 的固定进程映射"; return
        }
        if (activeSession != null || activeRuntime != null) { error = "此插件控制器已有活动 Session"; return }
        try { ExtensionRelayBootstrap.build(launchRequest) }
        catch (failure: IllegalArgumentException) { error = failure.message ?: "插件启动参数无效"; return }
        launchBusy = true; error = null
        viewModelScope.launch {
            try {
                val decoded = owner.decodeClientArguments(entry.review.id, entry.review.digest,
                    command.path, launchRequest.arguments)
                val id = sessions.connectExtensionRoute(target, launchRequest) { ready ->
                    try {
                        check(ready.info.phase == 1) { "SSH 插件通道尚未就绪" }
                        val extraCapabilities = buildList {
                            if (request.workspaceRootDirectory != null) add("file.readScoped")
                            if (request.processPrograms.isNotEmpty()) {
                                add("process.spawn")
                                add("process.pipes")
                            }
                        }
                        val adopted = owner.adoptSSHSession(entry.review.id, entry.review.digest,
                            request.executorDigest, "0.1.0", target.alias,
                            (launchRequest.methods.map { it to ExtensionSession.Role.EXECUTOR } +
                                launchRequest.hostMethods.map { it to ExtensionSession.Role.HOST }).map { (method, receiver) ->
                                ExtensionSession.Method(method, "session.rpc", receiver,
                                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)
                            }, additionalRequiredCapabilities = extraCapabilities)
                        activeSession = ActiveSession(entry.review.id, entry.review.digest, command,
                            launchRequest.arguments.toList(), decoded, launchRequest.methods.toList(), launchRequest.hostMethods.toList(),
                            launchRequest.executorDigest, target.alias, ExtensionRelayBootstrap.build(launchRequest),
                            (entry.review.requiredCapabilities + extraCapabilities).distinct(), adopted.info)
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
                    while (sessions.connecting?.id == id) delay(25)
                    if (activeSession == null) launchBusy = false
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "插件参数校验或启动失败：${failure.message ?: failure.javaClass.simpleName}"
                launchBusy = false
            }
        }
    }

    fun reportError(message: String) { error = message }

    fun editCodexMessage(value: String) { codex = codex.copy(messageDraft = value) }

    fun startCodexThread() {
        if (!canCallCodex("codex.thread.start") || codex.threadID != null) return
        callCodex("无法启动 Codex 对话") {
            val value = codexRPC("codex.thread.start", JSONObject())
            val id = value.optString("threadID", "")
            require(id.isNotBlank() && id.toByteArray().size <= 128) { "Codex thread ID 无效" }
            codex = codex.copy(threadID = id)
        }
    }

    fun sendCodexMessage() {
        if (!canCallCodex("codex.turn.send") || codex.threadID == null || codex.turnActive) return
        val text = codex.messageDraft.trim()
        if (text.isEmpty() || text.toByteArray().size > 2048) return
        callCodex("Codex 消息发送失败") {
            val input = JSONObject().put("text", text)
            codexRPC("codex.turn.send", input)
            codex = codex.copy(messageDraft = "")
        }
    }

    fun interruptCodexTurn() {
        if (!canCallCodex("codex.turn.interrupt") || !codex.turnActive) return
        callCodex("Codex 中断失败") {
            val value = codexRPC("codex.turn.interrupt", JSONObject())
            check(value.optBoolean("accepted", false)) { "Executor 未接受中断请求" }
        }
    }

    fun respondCodexApproval(id: String, allow: Boolean) {
        if (!canCallCodex("codex.approval.respond") || codex.approvals.none { it.id == id }) return
        callCodex("Codex 审批响应失败") {
            val decision = JSONObject.quote(if (allow) "accept" else "decline")
            val value = codexRPC("codex.approval.respond", JSONObject()
                .put("requestID", id).put("decisionJSON", decision))
            check(value.optBoolean("accepted", false)) { "Executor 未接受审批决定" }
            codex = codex.copy(approvals = codex.approvals.filterNot { it.id == id })
        }
    }

    private fun canCallCodex(method: String): Boolean = !codex.callBusy &&
        activeSession?.command?.path == "ai codex" && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE &&
        method in activeSession?.methods.orEmpty()

    private fun callCodex(message: String, block: suspend () -> Unit) {
        if (codex.callBusy) return
        viewModelScope.launch {
            codex = codex.copy(callBusy = true)
            try { block() }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { error = "$message：${failure.message ?: failure.javaClass.simpleName}" }
            finally { codex = codex.copy(callBusy = false) }
        }
    }

    private suspend fun codexRPC(method: String, input: JSONObject): JSONObject {
        val owner = controller ?: error("插件 Host 不可用")
        check(method in activeSession?.methods.orEmpty()) { "Codex RPC 未在本次 Session allowlist 中" }
        val admission = owner.requestRuntimeExecutor(method, input.toString().toByteArray(Charsets.UTF_8), 30000)
        check(admission.status == ExtensionSession.RPCStatus.OK) { "Codex RPC admission 失败：${admission.status}" }
        val handle = checkNotNull(admission.handle)
        pendingCodexRPCHandles += handle
        activeCodexRPCHandles += handle
        var confirmed = false
        try {
            val deadline = SystemClock.elapsedRealtime() + 35000
            while (SystemClock.elapsedRealtime() < deadline) {
                val (status, result) = owner.runtimeExecutorRequestResult(handle)
                check(status == ExtensionSession.RPCStatus.OK) { "Codex RPC result 失败：$status" }
                if (result?.confirmed == true) {
                    confirmed = true
                    check(result.state == ExtensionSession.RPCState.SUCCEEDED) {
                        "Codex RPC 结果未成功确认：${result.state}"
                    }
                    return CodexHostEvents.decodeOutcome(result.outcome)
                }
                delay(20)
            }
            error("Codex RPC 超时；结果未知，不会自动重试")
        } finally {
            activeCodexRPCHandles -= handle
            if (confirmed) {
                val forgotten = owner.forgetRuntimeExecutorRequest(handle)
                if (forgotten != ExtensionSession.RPCStatus.OK) error("无法释放 Codex RPC 结果句柄：$forgotten")
                pendingCodexRPCHandles -= handle
            } else {
                owner.cancelRuntimeExecutorRequest(handle)
            }
        }
    }

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
                activeRuntime = null; activeSession = null; receivedResources = emptyList()
                codex = CodexHostState(); pendingCodexRPCHandles.clear(); activeCodexRPCHandles.clear()
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
                reapCodexRPCs(owner)
                val (resourceStatus, content) = owner.takeRuntimeResource()
                if (resourceStatus == ExtensionResourceStore.Status.OK && content != null) {
                    val resources = receivedResources
                    if (resources.none { it.descriptor.resourceId == content.descriptor.resourceId }) {
                        receivedResources = (resources + content).takeLast(8)
                    }
                } else if (resourceStatus !in setOf(ExtensionResourceStore.Status.WAIT,
                        ExtensionResourceStore.Status.NOT_FOUND, ExtensionResourceStore.Status.CLOSED,
                        ExtensionResourceStore.Status.UNSUPPORTED)) {
                    error = "读取插件资源失败：$resourceStatus"
                }
                if (state.command == "ai codex" && state.info.phase == ExtensionRuntime.Phase.ACTIVE) {
                    pumpCodexHostEvent(owner)
                }
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

    private suspend fun pumpCodexHostEvent(owner: ExtensionController) {
        val (status, request) = owner.takeRuntimeRequest()
        if (status == ExtensionSession.RPCStatus.WAIT) return
        check(status == ExtensionSession.RPCStatus.OK && request != null) { "读取 Codex Host RPC 失败：$status" }
        if (request.method != "codex.event" || "codex.event" !in activeSession?.hostMethods.orEmpty()) {
            respondCodexEvent(owner, request.ticket, false, "Host method is not registered")
            return
        }
        val next = try {
            CodexHostEvents.accept(codex, CodexHostEvents.decode(request))
        } catch (failure: Exception) {
            respondCodexEvent(owner, request.ticket, false,
                failure.message?.take(256) ?: "Malformed Codex event")
            owner.requestStopRuntime()
            throw IllegalStateException("Codex Executor event rejected: ${failure.message}", failure)
        }
        respondCodexEvent(owner, request.ticket, true, null)
        codex = next
    }

    private suspend fun respondCodexEvent(owner: ExtensionController, ticket: Long, accepted: Boolean, message: String?) {
        val outcome = if (accepted) JSONObject().put("kind", "success")
            .put("value", JSONObject().put("accepted", true))
        else JSONObject().put("kind", "failed").put("error", JSONObject()
            .put("code", "DENIED").put("message", message ?: "Denied").put("effect", "none"))
        check(owner.respondRuntimeRequest(ticket, outcome.toString().toByteArray(Charsets.UTF_8)) ==
            ExtensionSession.RPCStatus.OK) { "无法确认 Codex Host RPC" }
    }

    private suspend fun reapCodexRPCs(owner: ExtensionController) {
        for (handle in pendingCodexRPCHandles.toList()) {
            if (handle in activeCodexRPCHandles) continue
            val (status, result) = owner.runtimeExecutorRequestResult(handle)
            if (status == ExtensionSession.RPCStatus.CLOSED) {
                pendingCodexRPCHandles -= handle
                continue
            }
            if (status != ExtensionSession.RPCStatus.OK || result == null || !result.confirmed) continue
            val statusAfter = owner.forgetRuntimeExecutorRequest(handle)
            if (statusAfter == ExtensionSession.RPCStatus.OK) pendingCodexRPCHandles -= handle
            else error = "无法释放终态 Codex RPC 句柄：$statusAfter"
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

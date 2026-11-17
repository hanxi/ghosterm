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
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/** Per-AppModel review state. Trust is deliberately process-local in this slice. */
class ExtensionPackagesViewModel internal constructor(application: Application,
    internal val controller: ExtensionController?) : AndroidViewModel(application) {
    constructor(application: Application) : this(application, createController(application))
    private val codexExecutorMethods = listOf("codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond", "codex.change.read")
    private val codexHostMethods = listOf("codex.event")
    private val pendingExtensionRPCHandles = mutableSetOf<Long>()
    private val activeExtensionRPCHandles = mutableSetOf<Long>()
    private var sessionGeneration = Any()
    private var rpcGeneration = Any()
    private var claudeRPC: ClaudeRPCClient? = null
    internal var claudeConversation: ClaudeConversation? by mutableStateOf(null)
        private set
    internal var table: PluginTableModel? by mutableStateOf(null)
        private set
    internal var diff: PluginDiffModel? by mutableStateOf(null)
        private set
    internal var task: PluginTaskModel? by mutableStateOf(null)
        private set
    internal var registeredViews: PluginViewsModel? by mutableStateOf(null)
        private set
    private var handshakeJob: Job? = null
    internal val changeReview = CodexChangeReviewController(viewModelScope)
    data class Entry(val review: ExtensionController.Review, val decision: String = "尚未批准")
    data class ActiveSession(
        val packageID: String, val packageDigest: String, val command: ExtensionController.Command,
        val arguments: List<String>, val decodedArguments: String?, val methods: List<String>, val hostMethods: List<String>, val executorDigest: String,
        val peerIdentity: String, val remoteCommand: String, val requiredCapabilities: List<String>,
        val info: ExtensionSession.Info,
        val executorExecution: ExtensionRelayBootstrap.Execution? = null,
        val clientWorkspace: ExtensionController.ClientWorkspace? = null,
        val executorAttachment: ExtensionRelayBootstrap.Attachment? = null,
    )

    companion object {
        private fun createController(application: Application): ExtensionController? = if (BuildConfig.PLUGIN_JS_ENABLED && ExtensionRuntime.supportStatus() == 0) {
        runCatching {
            ExtensionController(
                ExtensionController.HostProfile(
                    sdkVersion = "1.0.0", protocols = listOf(1), runtimes = listOf("portable-js"),
                    capabilities = listOf("session.rpc", "resource.receive", "file.readScoped", "ui.table", "ui.diff", "ui.task", "ui.views")),
                application.applicationContext)
        }.getOrNull()
    } else null

    }

    internal val connectionGroups = androidx.compose.runtime.mutableStateListOf<ExtensionConnectionModel>()
    internal var groupLaunchBusy by mutableStateOf(false)
        private set
    internal val pendingGroup = androidx.compose.runtime.mutableStateListOf<ExtensionGroupInvocation>()
    internal var selectedGroupInstance: ExtensionPackagesViewModel? by mutableStateOf(null)
        private set
    internal fun selectGroupInstance(instance: ExtensionPackagesViewModel?) { selectedGroupInstance = instance }

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

    internal suspend fun inspectForeground(target: PreparedSSH, executable: String, directory: String,
        instance: String, sessions: SessionsViewModel): PluginForegroundDeclaration {
        val owner = checkNotNull(controller) { "此构建未启用插件运行时" }
        check(!launchBusy && activeSession == null && activeRuntime == null && sessions.connecting == null) { "已有插件运行或 SSH 连接请求" }
        PluginForegroundDeclaration.bootstrap(executable, directory, instance)
        launchBusy = true; error = null
        var attempt: SSHAttempt? = null
        var result: PluginForegroundDeclaration? = null
        try {
            val id = checkNotNull(sessions.inspectExtensionRoute(target, executable, directory, instance) {
                val value = owner.readForegroundDeclaration(instance)
                owner.closeSSHChannel()
                result = value
                true
            }) { "无法开始前台实例检查" }
            attempt = sessions.connecting?.takeIf { it.id == id }
            while (sessions.connecting?.id == id) {
                currentCoroutineContext().ensureActive()
                val current = checkNotNull(sessions.connecting)
                check(!current.failed) { current.message }
                delay(25)
            }
            currentCoroutineContext().ensureActive()
            return checkNotNull(result) { "前台检查已取消或未确认在线" }
        } finally {
            withContext(NonCancellable) {
                attempt?.let { current ->
                    if (sessions.connecting === current) sessions.cancelConnection(current.id)
                    current.job?.join()
                }
            }
            launchBusy = false
        }
    }

    fun launch(entry: Entry, command: ExtensionController.Command, target: PreparedSSH,
        request: ExtensionRelayBootstrap.Request, sessions: SessionsViewModel,
        clientWorkspace: ExtensionController.ClientWorkspace? = null) {
        val owner = controller ?: return
        val prepared = try { prepareLaunch(entry, command, request, clientWorkspace) }
        catch (failure: IllegalArgumentException) { error = failure.message; return }
        val selectedCommand = prepared.command; val launchRequest = prepared.request
        if (launchBusy) return
        if (activeSession != null) { error = "请先停止并释放当前插件 Session。"; return }
        if (sessions.connecting != null) { error = "当前已有 SSH 连接请求，请完成或取消后再启动插件"; return }
        try { ExtensionRelayBootstrap.build(launchRequest) }
        catch (failure: IllegalArgumentException) { error = failure.message; return }
        launchBusy = true; error = null
        viewModelScope.launch {
            try {
                val exact = owner.resolve(selectedCommand.path, entry.review.id, entry.review.digest)
                check(exact.status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK && exact.command == selectedCommand) {
                    "命令声明已变化，请重新核对插件来源"
                }
                val decoded = owner.decodeClientArguments(entry.review.id, entry.review.digest,
                    selectedCommand.path, launchRequest.arguments)
                val id = sessions.connectExtensionRoute(target, launchRequest) { ready ->
                    try {
                        check(ready.info.phase == 1) { "SSH 插件通道尚未就绪" }
                        adoptReadyInstance(owner, prepared, decoded, target.alias, ExtensionRelayBootstrap.build(launchRequest))
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

    internal data class PreparedLaunch(val entry: Entry, val command: ExtensionController.Command,
        val request: ExtensionRelayBootstrap.Request, val clientWorkspace: ExtensionController.ClientWorkspace?)
    internal fun prepareLaunch(entry: Entry, command: ExtensionController.Command, request: ExtensionRelayBootstrap.Request,
        clientWorkspace: ExtensionController.ClientWorkspace?): PreparedLaunch {
        val selectedCommand = command.copy(views = command.views.toList())
        val registeredContribution = "ui.views" in entry.review.requiredCapabilities
        val tableContribution = "table" in selectedCommand.views
        val diffContribution = "diff" in selectedCommand.views
        val taskContribution = "task" in selectedCommand.views
        if (request.command != selectedCommand.path || entry.review.commands.firstOrNull { it.path == selectedCommand.path } != selectedCommand ||
            (tableContribution && !entry.review.requiredCapabilities.containsAll(listOf("session.rpc", "ui.table"))) ||
            (diffContribution && !entry.review.requiredCapabilities.containsAll(listOf("session.rpc", "ui.diff"))) ||
            (taskContribution && !entry.review.requiredCapabilities.containsAll(listOf("session.rpc", "ui.task"))) ||
            (registeredContribution && !tableContribution && !diffContribution && !taskContribution) ||
            request.clientCapabilities.distinct().size != request.clientCapabilities.size ||
            request.clientCapabilities.any { when (it) { "ui.table" -> !tableContribution; "ui.diff" -> !diffContribution; "ui.task" -> !taskContribution; "ui.views" -> !registeredContribution; else -> true } }) {
            throw IllegalArgumentException("只能启动已批准的准确命令与已声明的 Client 服务")
        }
        val baseRequest = if (selectedCommand.path == "ai codex") request.copy(
            methods = (request.methods + codexExecutorMethods).distinct(),
            hostMethods = (request.hostMethods + codexHostMethods).distinct())
        else if (selectedCommand.path == "ai claude") request.copy(methods = (request.methods + ClaudeRPCClient.methods).distinct())
        else request
        val tableRequest = if (!registeredContribution && tableContribution) baseRequest.copy(
            methods = (baseRequest.methods + PluginTableModel.methods).distinct(), clientCapabilities = listOf("ui.table")) else baseRequest
        val diffRequest = if (!registeredContribution && diffContribution) tableRequest.copy(
            methods = (tableRequest.methods + PluginDiffModel.methods).distinct(), clientCapabilities = (tableRequest.clientCapabilities + "ui.diff").distinct()) else tableRequest
        val taskRequest = if (!registeredContribution && taskContribution) diffRequest.copy(
            methods = (diffRequest.methods + PluginTaskModel.methods).distinct(), clientCapabilities = (diffRequest.clientCapabilities + "ui.task").distinct()) else diffRequest
        val configuredRequest = if (registeredContribution) baseRequest.copy(clientCapabilities = buildList {
            if (tableContribution) add("ui.table")
            if (diffContribution) add("ui.diff")
            if (taskContribution) add("ui.task")
            add("ui.views")
        }) else taskRequest
        val launchRequest = configuredRequest.copy(arguments = configuredRequest.arguments.toList(),
            methods = configuredRequest.methods.toList(), hostMethods = configuredRequest.hostMethods.toList(),
            processPrograms = configuredRequest.processPrograms.map { it.copy(prefixArguments = it.prefixArguments.toList()) },
            processEnvironmentNames = configuredRequest.processEnvironmentNames.toList(),
            clientCapabilities = configuredRequest.clientCapabilities.toList())
        if (entry.decision != "已批准（本进程）" || entry.review.digest != launchRequest.clientDigest ||
            entry.review.commands.none { it.path == selectedCommand.path }) {
            throw IllegalArgumentException("只能启动本进程已批准摘要中的命令")
        }
        if ((launchRequest.methods.isNotEmpty() || launchRequest.hostMethods.isNotEmpty()) &&
            "session.rpc" !in entry.review.requiredCapabilities) {
            throw IllegalArgumentException("此 Client 包未声明 session.rpc，不能协商 RPC 方法")
        }
        if (selectedCommand.path == "ai codex" && launchRequest.attachment == null && (launchRequest.workspaceRootDirectory.isNullOrBlank() ||
                launchRequest.processPrograms.none { it.name == "codex" })) {
            throw IllegalArgumentException("ai codex 需要明确授权工作区，并配置名为 codex 的固定进程映射")
        }
        if (selectedCommand.path == "ai claude" && launchRequest.attachment == null && (launchRequest.workspaceRootDirectory.isNullOrBlank() ||
                launchRequest.processPrograms.none { it.name == "claude-node" })) {
            throw IllegalArgumentException("ai claude 需要明确授权工作区，并配置名为 claude-node 的固定进程映射")
        }
        if (selectedCommand.path in listOf("ai codex", "ai claude") && launchRequest.attachment != null && !launchRequest.attachment.remoteProcesses) {
            throw IllegalArgumentException("此命令需要明确允许沿用前台实例已有的远端业务进程范围")
        }
        clientWorkspace?.validate(entry.review.requiredCapabilities)
        if (launchRequest.attachment != null) ExtensionRelayBootstrap.build(launchRequest)
        return PreparedLaunch(entry.copy(review = entry.review.copy(requiredCapabilities = entry.review.requiredCapabilities.toList(),
            commands = entry.review.commands.map { it.copy(views = it.views.toList()) })), selectedCommand, launchRequest, clientWorkspace)
    }

    private suspend fun adoptReadyInstance(owner: ExtensionController, prepared: PreparedLaunch, decoded: String?,
        peerIdentity: String, remoteCommand: String) {
        val entry = prepared.entry; val selectedCommand = prepared.command
        val launchRequest = prepared.request; val clientWorkspace = prepared.clientWorkspace
        val extraCapabilities = buildList {
            if ((launchRequest.workspaceRootDirectory != null || clientWorkspace != null || launchRequest.attachment?.scopedFiles == true) &&
                "resource.receive" in entry.review.requiredCapabilities) add("file.readScoped")
            if (launchRequest.processPrograms.isNotEmpty() || launchRequest.attachment?.remoteProcesses == true) {
                add("process.spawn")
                add("process.pipes")
            }
        }
        val adopted = owner.adoptSSHSession(entry.review.id, entry.review.digest,
            launchRequest.executorDigest, "0.1.0", peerIdentity,
            (launchRequest.methods.map { it to ExtensionSession.Role.EXECUTOR } +
                launchRequest.hostMethods.map { it to ExtensionSession.Role.HOST }).map { (method, receiver) ->
                ExtensionSession.Method(method, "session.rpc", receiver,
                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)
            }, additionalRequiredCapabilities = extraCapabilities)
        sessionGeneration = Any(); rpcGeneration = Any(); changeReview.cancel()
        claudeRPC?.invalidate(); claudeRPC = null
        claudeConversation?.invalidate(); claudeConversation = null
        table?.invalidate(); table = null
        diff?.invalidate(); diff = null
        registeredViews?.invalidate(); task?.invalidate(); task = null; registeredViews = null
        codex = CodexHostState(); pendingExtensionRPCHandles.clear(); activeExtensionRPCHandles.clear()
        activeSession = ActiveSession(entry.review.id, entry.review.digest, selectedCommand,
            launchRequest.arguments.toList(), decoded, launchRequest.methods.toList(), launchRequest.hostMethods.toList(),
            launchRequest.executorDigest, peerIdentity, remoteCommand,
            (entry.review.requiredCapabilities + extraCapabilities).distinct(), adopted.info, launchRequest.execution, clientWorkspace, launchRequest.attachment)
        launchBusy = false
        handshakeJob?.cancel()
        handshakeJob = viewModelScope.launch { advanceToConsent(owner) }
    }

    internal suspend fun startMultiplexInstance(prepared: PreparedLaunch, peerIdentity: String, remoteCommand: String) {
        val owner = checkNotNull(controller)
        check(activeSession == null && activeRuntime == null)
        val exact = owner.resolve(prepared.command.path, prepared.entry.review.id, prepared.entry.review.digest)
        check(exact.status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK && exact.command == prepared.command) { "命令声明已变化，请重新核对来源" }
        val decoded = owner.decodeClientArguments(prepared.entry.review.id, prepared.entry.review.digest, prepared.command.path, prepared.request.arguments)
        adoptReadyInstance(owner, prepared, decoded, peerIdentity, remoteCommand)
    }

    internal suspend fun closeInstance() {
        handshakeJob?.cancel(); handshakeJob?.join(); handshakeJob = null
        sessionGeneration = Any(); invalidateExtensionCalls()
        controller?.close()
        activeRuntime = null; activeSession = null; receivedResources = emptyList()
        claudeConversation = null; table = null; diff = null; task = null; registeredViews = null
        launchBusy = false
    }

    internal fun enqueueGroup(entry: Entry, command: ExtensionController.Command, alias: String,
        request: ExtensionRelayBootstrap.Request, clientWorkspace: ExtensionController.ClientWorkspace?, temporaryDirectory: String) {
        try {
            check(pendingGroup.size < 8) { "连接组最多八项" }
            val prepared = prepareLaunch(entry, command, request, clientWorkspace)
            check(prepared.request.attachment == null) { "已有前台附着不能加入固定启动组" }
            val candidate = ExtensionGroupInvocation(alias, prepared, temporaryDirectory)
            val group = pendingGroup.toList() + candidate
            require(group.all { it.alias == alias }) { "同一连接组必须选择同一目标" }
            ExtensionRelayBootstrap.buildMultiplex("1".repeat(32), group.mapIndexed { index, item ->
                ExtensionController.MultiplexScope(item.id.toString().replace("-", ""), (index + 1).toULong())
            }, group.map { it.launch.request }, temporaryDirectory)
            require(group.all { it.temporaryDirectory == temporaryDirectory }) { "同一连接组必须使用相同临时目录" }
            pendingGroup.add(candidate)
        } catch (failure: Exception) { error = failure.message ?: "连接组配置无效" }
    }

    internal fun launchGroup(target: PreparedSSH, sessions: SessionsViewModel) {
        val snapshot = pendingGroup.toList()
        if (groupLaunchBusy || snapshot.isEmpty() || sessions.connecting != null || connectionGroups.any { it.busy }) return
        val owner = controller ?: return
        if (connectionGroups.count { !it.closed } >= 16) { error = "插件连接数量已达上限"; return }
        groupLaunchBusy = true
        viewModelScope.launch {
            try {
                val group = ExtensionConnectionModel(getApplication(), owner.makeInvocationController(), viewModelScope)
                connectionGroups.add(group)
                group.start(snapshot, target, sessions)
                if (group.instances.isNotEmpty() && !group.closed) pendingGroup.removeAll(snapshot.toSet())
                if (selectedGroupInstance == null) selectedGroupInstance = group.instances.firstOrNull()?.model
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { error = "连接组启动失败：${failure.message}" }
            finally { groupLaunchBusy = false }
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

    internal fun codexApprovalSnapshot(approval: CodexPendingApproval, review: CodexChangeReviewState? = changeReview.state.value) =
        if (approval.method == "item/permissions/requestApproval") approval.permissionReviewID else approval.identity?.reviewedSnapshot(review, ::currentChange)

    fun respondCodexApproval(id: String, allow: Boolean, snapshotID: String? = null) {
        if (!canCallCodex("codex.approval.respond")) return
        val approval = codex.approvals.firstOrNull { it.id == id } ?: return
        val fileApproval = approval.method == "item/fileChange/requestApproval"
        val permissionApproval = approval.method == "item/permissions/requestApproval"
        if (permissionApproval && (snapshotID == null || snapshotID != approval.permissionReviewID)) {
            error = "权限请求已更新，请重新核对当前完整权限范围。"
            return
        }
        if (allow && fileApproval && (snapshotID == null || snapshotID != codexApprovalSnapshot(approval))) {
            error = "请先读取并核对当前完整补丁，再批准此版本。"
            return
        }
        callCodex("Codex 审批响应失败") {
            val generation = rpcGeneration
            try {
                val decision = JSONObject.quote(if (allow) "accept" else "decline")
                val value = codexRPC("codex.approval.respond", JSONObject()
                    .put("requestID", id).put("decisionJSON", decision)
                    .put("snapshotID", if (permissionApproval || (allow && fileApproval)) snapshotID else ""))
                check(value.optBoolean("accepted", false)) { "Executor 未接受审批决定" }
                codex = codex.copy(approvals = codex.approvals.filterNot { it.id == id })
            } catch (failure: Exception) {
                if (generation === rpcGeneration && allow && fileApproval) changeReview.cancel()
                throw failure
            }
        }
    }

    private fun canCallCodex(method: String): Boolean = !launchBusy && !codex.callBusy &&
        activeSession?.command?.path == "ai codex" && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE &&
        method in activeSession?.methods.orEmpty()

    private fun callCodex(message: String, block: suspend () -> Unit) {
        if (codex.callBusy) return
        val generation = rpcGeneration
        codex = codex.copy(callBusy = true)
        viewModelScope.launch {
            try { block() }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (generation === rpcGeneration) error = "$message：${failure.message ?: failure.javaClass.simpleName}" }
            finally { if (generation === rpcGeneration) codex = codex.copy(callBusy = false) }
        }
    }

    private suspend fun codexRPC(method: String, input: JSONObject): JSONObject =
        CodexHostEvents.decodeOutcome(requestExtensionRPC(method, input.toString().toByteArray(Charsets.UTF_8)))

    internal suspend fun requestClaudeAction(method: String, parameters: ByteArray): ByteArray {
        check(!launchBusy && activeSession?.command?.path == "ai claude" &&
            activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE &&
            ClaudeRPCClient.methods.all { it in activeSession?.methods.orEmpty() }) {
            "Claude 操作需要当前 ACTIVE Runtime 和完整八方法 allowlist"
        }
        val client = claudeRPC ?: run {
            val generation = rpcGeneration
            ClaudeRPCClient { rpcMethod, bytes ->
                checkRPCGeneration(generation)
                requestExtensionRPC(rpcMethod, bytes).also { checkRPCGeneration(generation) }
            }.also { claudeRPC = it }
        }
        return client.request(method, parameters)
    }

    internal suspend fun observeClaudeOutcome(): ClaudeRPCClient.Observation {
        val generation = rpcGeneration
        checkRPCGeneration(generation)
        check(!launchBusy && activeSession?.command?.path == "ai claude" &&
            activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE &&
            ClaudeRPCClient.methods.all { it in activeSession?.methods.orEmpty() }) { "原 Claude Runtime 已不可观察" }
        val client = checkNotNull(claudeRPC) { "没有本次 Claude 操作适配器" }
        return client.observe().also { checkRPCGeneration(generation) }
    }

    internal suspend fun requestPluginViewOutcome(method: String, input: ByteArray): ByteArray =
        requestExtensionRPC(method, input, returnTerminalOutcome = true)

    private suspend fun requestExtensionRPC(method: String, input: ByteArray, returnTerminalOutcome: Boolean = false): ByteArray {
        val generation = rpcGeneration
        checkRPCGeneration(generation)
        val owner = controller ?: error("插件 Host 不可用")
        check(!launchBusy && method in activeSession?.methods.orEmpty()) { "插件 RPC 未在本次 Session allowlist 中" }
        val admission = withContext(NonCancellable) {
            owner.requestRuntimeExecutor(method, input, 30000)
        }
        if (returnTerminalOutcome && admission.status != ExtensionSession.RPCStatus.OK) {
            checkRPCGeneration(generation)
            val code = when (admission.status) {
                ExtensionSession.RPCStatus.WAIT -> "BUSY"
                ExtensionSession.RPCStatus.INVALID -> "INVALID_INPUT"
                ExtensionSession.RPCStatus.DENIED -> "DENIED"
                ExtensionSession.RPCStatus.UNSUPPORTED -> "UNSUPPORTED"
                ExtensionSession.RPCStatus.CLOSED -> "CANCELLED"
                else -> error("RPC admission 无效")
            }
            return JSONObject().put("kind", "failed").put("error", JSONObject().put("code", code).put("message", code).put("effect", "none")).toString().toByteArray(Charsets.UTF_8)
        }
        check(admission.status == ExtensionSession.RPCStatus.OK) { "插件 RPC admission 失败：${admission.status}" }
        val handle = checkNotNull(admission.handle)
        if (generation === rpcGeneration) {
            pendingExtensionRPCHandles += handle
            activeExtensionRPCHandles += handle
        }
        var confirmed = false
        var outcome: ByteArray? = null
        try {
            checkRPCGeneration(generation)
            val deadline = SystemClock.elapsedRealtime() + 35000
            while (SystemClock.elapsedRealtime() < deadline) {
                checkRPCGeneration(generation)
                val (status, result) = owner.runtimeExecutorRequestResult(handle)
                checkRPCGeneration(generation)
                check(status == ExtensionSession.RPCStatus.OK) { "插件 RPC result 失败：$status" }
                if (returnTerminalOutcome && result != null && result.state !in setOf(ExtensionSession.RPCState.QUEUED, ExtensionSession.RPCState.PENDING)) {
                    confirmed = true // terminal handle can be retired; business confirmation stays in its outcome
                    outcome = if (result.state == ExtensionSession.RPCState.SUCCEEDED && !result.confirmed)
                        """{"kind":"unknown","reason":"UNCONFIRMED_SUCCESS"}""".toByteArray() else result.outcome.copyOf()
                    break
                }
                if (result?.confirmed == true) {
                    confirmed = true
                    check(result.state == ExtensionSession.RPCState.SUCCEEDED) {
                        "插件 RPC 结果未成功确认：${result.state}"
                    }
                    outcome = result.outcome.copyOf()
                    break
                }
                delay(20)
            }
            check(outcome != null) { "插件 RPC 超时；结果未知，不会自动重试" }
        } finally {
            withContext(NonCancellable) {
                if (generation === rpcGeneration) {
                    activeExtensionRPCHandles -= handle
                    if (confirmed) {
                        val forgotten = owner.forgetRuntimeExecutorRequest(handle)
                        if (generation === rpcGeneration) {
                            if (forgotten != ExtensionSession.RPCStatus.OK) error("无法释放插件 RPC 结果句柄：$forgotten")
                            pendingExtensionRPCHandles -= handle
                        }
                    } else owner.cancelRuntimeExecutorRequest(handle)
                }
            }
        }
        checkRPCGeneration(generation)
        return checkNotNull(outcome)
    }

    private suspend fun checkRPCGeneration(generation: Any) {
        currentCoroutineContext().ensureActive()
        if (generation !== rpcGeneration) throw CancellationException("Plugin Session changed")
    }

    internal val canReadCodexChanges get() = !launchBusy && activeSession?.command?.path == "ai codex" &&
        activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE && "codex.change.read" in activeSession?.methods.orEmpty()

    internal fun loadCodexChanges(reference: CodexChangeReference) {
        if (!canReadCodexChanges || !currentChange(reference)) return
        val generation = rpcGeneration
        changeReview.load(reference,
            isCurrent = { generation === rpcGeneration && canReadCodexChanges && currentChange(reference) },
            read = { offset -> CodexChangeJson.chunk(codexRPC("codex.change.read",
                JSONObject().put("snapshotID", reference.snapshotID).put("offset", offset))) },
            decode = CodexChangeJson::document)
    }

    private fun currentChange(reference: CodexChangeReference) = codex.threadID == reference.threadID &&
        codex.timeline.any { it.changeReference == reference }

    private fun invalidateExtensionCalls() {
        table?.invalidate()
        diff?.invalidate()
        registeredViews?.invalidate(); task?.invalidate()
        claudeConversation?.invalidate()
        claudeRPC?.invalidate(); claudeRPC = null
        rpcGeneration = Any()
        changeReview.cancel()
        activeExtensionRPCHandles.clear()
        codex = codex.copy(callBusy = false)
    }

    fun decideConsent(allow: Boolean) {
        val owner = controller ?: return
        val current = activeSession ?: return
        if (launchBusy || current.info.phase != ExtensionSession.Phase.CONSENT || current.info.consentToken.isBlank()) return
        // Runtime activation transfers the native Session handle. Its old
        // consent poller must stop before that transfer, including a pending delay.
        handshakeJob?.cancel()
        handshakeJob = null
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
                                    active.command.path, active.arguments, active.clientWorkspace)
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
        if (launchBusy || activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.CREATED,
                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE)) return
        invalidateExtensionCalls()
        launchBusy = true
        owner.requestStopRuntime()
    }

    fun stopSession() {
        val owner = controller ?: return
        val current = activeSession ?: return
        if (launchBusy || current.info.phase != ExtensionSession.Phase.ACTIVE || activeRuntime != null) return
        invalidateExtensionCalls()
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
                sessionGeneration = Any(); invalidateExtensionCalls()
                activeRuntime = null; activeSession = null; receivedResources = emptyList()
                claudeConversation = null
                table = null; diff = null; task = null; registeredViews = null
                codex = CodexHostState(); pendingExtensionRPCHandles.clear(); activeExtensionRPCHandles.clear()
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

    private fun updateRegisteredViews(owner: ExtensionController, state: ExtensionController.RuntimeState) {
        state.viewIssue?.let { error = it }
        val active = activeSession ?: return
        if ("ui.views" !in active.requiredCapabilities || state.info.phase != ExtensionRuntime.Phase.ACTIVE) return
        try {
            val bytes = checkNotNull(state.views) { "视图注册快照缺失" }
            val views = registeredViews ?: run {
                val expected = rpcGeneration; val runtimeID = state.instanceID
                PluginViewsModel(viewModelScope, checkNotNull(state.viewSessionInstance), checkNotNull(state.viewSessionEpoch),
                    active.command.views.map { wire -> PluginViewRegistration.Kind.entries.first { it.wire == wire } }.toSet(),
                    active.requiredCapabilities.toSet(), active.methods.toSet(), request = { method, input ->
                        checkRPCGeneration(expected)
                        requestPluginViewOutcome(method, input).also { checkRPCGeneration(expected) }
                    }, isCurrent = {
                        expected === rpcGeneration && !launchBusy && activeRuntime?.instanceID == runtimeID && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE
                    }, close = { handle ->
                        checkRPCGeneration(expected)
                        val closed = owner.closeRuntimeView(runtimeID, handle)
                        checkRPCGeneration(expected)
                        checkNotNull(closed.views) { "关闭后的视图快照缺失" }
                    }).also { registeredViews = it }
            }
            views.apply(bytes)
        } catch (failure: Exception) {
            registeredViews?.invalidate()
            error = "插件视图注册不可用，可停止并释放：${failure.message}"
        }
    }

    private suspend fun advanceRuntime(owner: ExtensionController) {
        val generation = sessionGeneration
        try {
            while (true) {
                val state = owner.stepRuntime()
                if (generation !== sessionGeneration) return
                activeRuntime = state
                updateRegisteredViews(owner, state)
                if (state.info.phase == ExtensionRuntime.Phase.ACTIVE && table == null &&
                    "ui.views" !in activeSession?.requiredCapabilities.orEmpty() &&
                    "table" in activeSession?.command?.views.orEmpty() && "ui.table" in activeSession?.requiredCapabilities.orEmpty() &&
                    PluginTableModel.methods.all { it in activeSession?.methods.orEmpty() }) {
                    val expected = rpcGeneration
                    table = PluginTableModel(viewModelScope, { method, input ->
                        checkRPCGeneration(expected)
                        requestExtensionRPC(method, input).also { checkRPCGeneration(expected) }
                    }, isCurrent = {
                        expected === rpcGeneration && !launchBusy && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE
                    })
                }
                if (state.info.phase == ExtensionRuntime.Phase.ACTIVE && diff == null &&
                    "ui.views" !in activeSession?.requiredCapabilities.orEmpty() &&
                    "diff" in activeSession?.command?.views.orEmpty() && "ui.diff" in activeSession?.requiredCapabilities.orEmpty() &&
                    PluginDiffModel.methods.all { it in activeSession?.methods.orEmpty() }) {
                    val expected = rpcGeneration
                    diff = PluginDiffModel(viewModelScope, { method, input ->
                        checkRPCGeneration(expected)
                        requestExtensionRPC(method, input).also { checkRPCGeneration(expected) }
                    }, isCurrent = {
                        expected === rpcGeneration && !launchBusy && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE
                    })
                }
                if (state.info.phase == ExtensionRuntime.Phase.ACTIVE && task == null &&
                    "ui.views" !in activeSession?.requiredCapabilities.orEmpty() &&
                    "task" in activeSession?.command?.views.orEmpty() && "ui.task" in activeSession?.requiredCapabilities.orEmpty() &&
                    PluginTaskModel.methods.all { it in activeSession?.methods.orEmpty() }) {
                    val expected = rpcGeneration
                    task = PluginTaskModel(viewModelScope, { method, input ->
                        checkRPCGeneration(expected)
                        requestPluginViewOutcome(method, input).also { checkRPCGeneration(expected) }
                    }, isCurrent = {
                        expected === rpcGeneration && !launchBusy && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE
                    })
                }
                if (state.command == "ai claude" && state.info.phase == ExtensionRuntime.Phase.ACTIVE && claudeConversation == null &&
                    ClaudeRPCClient.methods.all { it in activeSession?.methods.orEmpty() }) {
                    val expected = rpcGeneration
                    claudeConversation = ClaudeConversation(viewModelScope, ::requestClaudeAction, isCurrent = {
                        expected === rpcGeneration && !launchBusy && activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE
                    }, observe = ::observeClaudeOutcome)
                }
                reapExtensionRPCs(owner)
                if (generation !== sessionGeneration) return
                val (resourceStatus, content) = owner.takeRuntimeResource()
                if (generation !== sessionGeneration) return
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
                        ExtensionRuntime.Phase.CREATED)) {
                    table?.invalidate()
                    diff?.invalidate()
                    registeredViews?.invalidate(); task?.invalidate()
                    claudeRPC?.invalidate()
                    claudeConversation?.invalidate()
                    codex = codex.copy(approvals = emptyList())
                    changeReview.cancel()
                    return
                }
                delay(50)
            }
        } catch (cancelled: CancellationException) {
            throw cancelled
        } catch (failure: Throwable) {
            if (generation !== sessionGeneration) return
            table?.invalidate()
            diff?.invalidate()
            registeredViews?.invalidate(); task?.invalidate()
            claudeRPC?.invalidate()
            claudeConversation?.invalidate()
            changeReview.cancel()
            codex = codex.copy(approvals = emptyList())
            error = "插件 Runtime 运行失败：${failure.message ?: failure.javaClass.simpleName}"
        } finally { if (generation === sessionGeneration) launchBusy = false }
    }

    private suspend fun pumpCodexHostEvent(owner: ExtensionController) {
        val generation = sessionGeneration
        val (status, request) = owner.takeRuntimeRequest()
        if (generation !== sessionGeneration) return
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
        if (generation !== sessionGeneration) return
        codex = next
        changeReview.invalidate(::currentChange)
        if (next.rawEvents.lastOrNull()?.let { JSONObject(it).optString("kind") } in
            setOf("protocol-error", "closed", "outcome-unknown")) changeReview.cancel()
    }

    private suspend fun respondCodexEvent(owner: ExtensionController, ticket: Long, accepted: Boolean, message: String?) {
        val outcome = if (accepted) JSONObject().put("kind", "success")
            .put("value", JSONObject().put("accepted", true))
        else JSONObject().put("kind", "failed").put("error", JSONObject()
            .put("code", "DENIED").put("message", message ?: "Denied").put("effect", "none"))
        check(owner.respondRuntimeRequest(ticket, outcome.toString().toByteArray(Charsets.UTF_8)) ==
            ExtensionSession.RPCStatus.OK) { "无法确认 Codex Host RPC" }
    }

    private suspend fun reapExtensionRPCs(owner: ExtensionController) {
        val generation = sessionGeneration
        for (handle in pendingExtensionRPCHandles.toList()) {
            if (handle in activeExtensionRPCHandles) continue
            val (status, result) = owner.runtimeExecutorRequestResult(handle)
            if (generation !== sessionGeneration) return
            if (status == ExtensionSession.RPCStatus.CLOSED) {
                pendingExtensionRPCHandles -= handle
                continue
            }
            if (status != ExtensionSession.RPCStatus.OK || result == null || !result.confirmed) continue
            val statusAfter = owner.forgetRuntimeExecutorRequest(handle)
            if (generation !== sessionGeneration) return
            if (statusAfter == ExtensionSession.RPCStatus.OK) pendingExtensionRPCHandles -= handle
            else error = "无法释放终态插件 RPC 句柄：$statusAfter"
        }
    }

    fun publishClientResource(relativePath: String, root: String = "client", instanceID: String? = activeRuntime?.instanceID) {
        val owner = controller ?: return
        if (launchBusy || activeRuntime?.info?.phase != ExtensionRuntime.Phase.ACTIVE) return
        val generation = sessionGeneration
        viewModelScope.launch {
            try {
                val state = owner.publishClientResource(instanceID.orEmpty(), relativePath, root)
                if (generation === sessionGeneration) activeRuntime = state
            } catch (failure: Throwable) {
                if (failure is CancellationException) throw failure
                if (generation === sessionGeneration) error = "资源发布失败：${failure.message ?: failure.javaClass.simpleName}"
            }
        }
    }
    fun cancelClientPublication(id: String) {
        val owner = controller ?: return
        if (activeRuntime?.publication?.id != id) return
        val generation = sessionGeneration
        viewModelScope.launch {
            try { owner.cancelClientPublication(id) }
            catch (failure: Throwable) {
                if (failure is CancellationException) throw failure
                if (generation === sessionGeneration) error = "无法取消资源发布：${failure.message ?: failure.javaClass.simpleName}"
            }
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
    /** A version switch never carries an old source decision into new admission. */
    internal fun versionSelectionChanged(result: ExtensionController.VersionSelectionResult) {
        entries = entries.map { if (it.review.id == result.pluginID) it.copy(decision = "尚未批准") else it }
        result.review?.let { reviewed ->
            if (entries.none { it.review.id == reviewed.id && it.review.digest == reviewed.digest }) entries = entries + Entry(reviewed)
        }
    }

    internal fun versionRemovalChanged(result: PluginReleaseRemoval.Result) {
        val removed = result.forgottenPackages.toSet()
        entries = entries.filterNot { (it.review.id to it.review.digest) in removed }
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

    override fun onCleared() {
        connectionGroups.forEach { it.dispose() }
        sessionGeneration = Any(); invalidateExtensionCalls()
        super.onCleared()
    }

}

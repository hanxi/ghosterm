package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import android.net.Uri
import android.os.SystemClock
import com.hanxi.ghosterm.data.IdentityDocumentReader
import java.net.Inet6Address
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.MutableStateFlow
import com.hanxi.ghosterm.data.ConfigRepository
import com.hanxi.ghosterm.data.FileTaskConnections
import com.hanxi.ghosterm.domain.FilePasteRequest
import com.hanxi.ghosterm.domain.FileEndpoint
import com.hanxi.ghosterm.terminal.TerminalSurface
import com.hanxi.ghosterm.terminal.TerminalAppearance
import com.hanxi.ghosterm.domain.AppPreferences
import java.io.File
import java.util.UUID

class TerminalSession(val id: UUID, val title: String, internal val connection: NativeTerminalSession,
    val remoteAlias: String? = null, val backgroundSession: String? = null,
    val remoteTarget: PreparedSSH? = null) {
    val surface: TerminalSurface get() = connection
    var state by mutableStateOf(SessionState(0, null, 0, null)); internal set
    internal var appliedAppearance: TerminalAppearance? = null
    internal var keyboardOffered = false
}

/** Activity ViewModel lifetime survives rotation; navigating never closes a session. */
class SSHAttempt(val target: PreparedSSH, internal val filesReady: ((RemoteFileSession, String) -> Unit)? = null) {
    internal var tmuxReady: ((RemoteTmuxSession,String)->Unit)? = null
    internal var tmuxTarget: RemoteTmuxTarget? = null
    internal var tmuxName: String? = null
    internal var taskLabel: String? = null
    internal var taskCancelled: (() -> Unit)? = null
    val passiveProgress get() = !failed && username.isNotBlank() &&
        auth?.phase != SSHAuthPhase.HOSTKEY && (auth?.phase != SSHAuthPhase.CREDENTIALS || automaticFileAuthentication)
    internal var automaticFileAuthentication by mutableStateOf(false)
    val forFiles get() = filesReady != null || taskLabel != null
    val id: UUID = UUID.randomUUID()
    var username by mutableStateOf(target.user)
    var message by mutableStateOf("正在解析主机地址…")
    var failed by mutableStateOf(false)
    var auth by mutableStateOf<SSHAuthState?>(null)
    var identities by mutableStateOf<List<StoredIdentity>>(emptyList())
    var credentialBusy by mutableStateOf(false)
    var credentialError by mutableStateOf<String?>(null)
    internal var connection: AuthenticatedSSH? = null
    internal var job: Job? = null
    internal var credentialJob: Job? = null
    internal var manualCredentials = false
    internal val automaticKeyRequests = mutableSetOf<Long>()
    internal val triedIdentityIds = mutableSetOf<String>()
    internal var submittedIdentityId: String? = null
    internal var manuallySubmittedIdentity = false
    var rememberIdentity by mutableStateOf(true)
    internal var rememberedIdentityId: String? = null
    val authenticationIdentityFiles: List<String> get() = rememberedIdentityId?.let {
        listOf(com.hanxi.ghosterm.data.IdentityReferences.encode(it))
    } ?: target.identityFiles
}

class SessionsViewModel @JvmOverloads constructor(application: Application, private val resolver: SSHResolver = AndroidSSHResolver,
    private val connectionBudgetMillis: Long = 60_000, private val monitorSessions: Boolean = true,
    settings: AppSettings? = (application as? GhostermApplication)?.settings) : AndroidViewModel(application) {
    private val processTerminals = monitorSessions && application is GhostermApplication
    private val terminalStore = if (processTerminals) (application as GhostermApplication).terminalStore else TerminalSessionStore()
    val sessions = terminalStore.sessions
    private val surfaceTargets = MutableStateFlow<List<TerminalSession>>(sessions.toList())
    var selected: UUID?
        get() = terminalStore.selected
        private set(value) { terminalStore.selected = value }
    var visible by mutableStateOf(false); private set
    var creating by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var identityMemoryWarning by mutableStateOf<String?>(null); private set
    fun dismissIdentityMemoryWarning() { identityMemoryWarning = null }
    private var sequence: Int
        get() = terminalStore.sequence
        set(value) { terminalStore.sequence = value }
    var connecting by mutableStateOf<SSHAttempt?>(null); private set
    var selectingNewRemote by mutableStateOf(false); private set
    var choosingRemote by mutableStateOf<String?>(null); private set
    fun dismissRemoteChoices() { choosingRemote = null }
    private var pendingIdentityImport: Pair<UUID,SSHAuthState>? = null
    private var pendingIdentityPresenter: UUID? = null
    var identityPickerPending by mutableStateOf(false); private set
    private val vault = IdentityVault(File(application.filesDir, "identities"))
    private val rememberedIdentities = com.hanxi.ghosterm.data.RememberedSSHIdentities(
        application.getSharedPreferences("ssh-successful-identities", android.content.Context.MODE_PRIVATE))
    val active: TerminalSession? get() {
        val id = selected ?: return null
        // Readers may run while the main thread registers a session. Iterate
        // one immutable SnapshotStateList view instead of its live iterator.
        return sessions.toList().firstOrNull { it.id == id }
    }
    private val background get() = if (processTerminals) (getApplication<Application>() as GhostermApplication).terminalBackground else null
    val backgroundState get() = background?.state
    val backgroundError get() = background?.error
    val backgroundAwake get() = background?.keepAwake == true
    val backgroundNotificationsVisible get() = background?.notificationsVisible != false
    fun protectInBackground() { background?.startFromUser(this) }
    fun toggleBackgroundAwake() { background?.setKeepAwakeFromUser(!backgroundAwake) }
    fun dismissBackgroundError() { background?.dismissError() }
    var terminalStyle by mutableStateOf((settings?.state?.value?.committed ?: AppPreferences()).terminalStyle()); private set
    var terminalControls by mutableStateOf((settings?.state?.value?.committed ?: AppPreferences()).terminalControls()); private set
    init {
        if (settings != null) viewModelScope.launch {
            settings.state.collect { terminalControls = it.committed.terminalControls() }
        }
        viewModelScope.launch {
            val styles = settings?.state?.map { state ->
                try {
                    withContext(Dispatchers.IO) { com.hanxi.ghosterm.terminal.TerminalFonts.prepare(application,state.committed.fontFamily.id) }
                    state.committed.terminalStyle()
                } catch(cancelled:CancellationException) { throw cancelled }
                catch(failure:Exception) { reportError("终端字体无法载入：${failure.message ?: "未知原因"}，请在设置中重新选择。");null }
            }?.filterNotNull()?.distinctUntilChanged()
                ?: flowOf(AppPreferences().terminalStyle())
            combine(styles, surfaceTargets) { style, targets -> style to targets }
                .collect { (style, targets) ->
                    terminalStyle = style
                    for (session in targets) {
                        if (session.appliedAppearance == style.appearance) continue
                        try {
                            session.surface.configure(style.appearance)
                            session.appliedAppearance = style.appearance
                        } catch (cancelled: CancellationException) { throw cancelled }
                        catch (failure: Exception) {
                            if (session in sessions) reportError("终端设置未能应用：${failure.message ?: "未知原因"}")
                        }
                    }
                }
        }
        if (processTerminals) (application as GhostermApplication).terminalBackground
        if(monitorSessions && !processTerminals) viewModelScope.launch {
            while (true) {
                sessions.forEach { it.state = it.connection.state() }
                delay(250)
            }
        }
    }
    fun local() {
        val previous = active?.takeIf { it.remoteAlias == null && !it.state.terminal }
            ?: sessions.lastOrNull { it.remoteAlias == null && !it.state.terminal }
        if (previous == null) newLocal() else show(previous.id)
    }
    fun newLocal() {
        if (creating) return
        creating = true; error = null
        protectInBackground()
        viewModelScope.launch {
            var created: LocalSession? = null
            try {
                withContext(Dispatchers.IO) {
                    val home = File(getApplication<Application>().filesDir, "home")
                    check(home.isDirectory || home.mkdirs()) { "无法创建本地终端目录" }
                    created = LocalSession(home.absolutePath)
                }
                val session = TerminalSession(UUID.randomUUID(), "本地终端 ${++sequence}", checkNotNull(created))
                register(session); created = null
                show(session.id)
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) { error = failure.message ?: "无法创建本地终端" }
            finally { created?.close(); creating = false }
        }
    }
    fun show(id: UUID) { if (sessions.any { it.id == id }) { selected = id; visible = true; error = null; choosingRemote = null } }
    fun home() { visible = false }
    fun reportError(message: String) { error = message }
    fun dismissError() { error = null }
    private fun register(session: TerminalSession) {
        sessions.add(session)
        surfaceTargets.value = sessions.toList()
    }
    fun close(id: UUID) {
        val session = sessions.firstOrNull { it.id == id } ?: return
        session.connection.close(); sessions.remove(session)
        surfaceTargets.value = sessions.toList()
        if (selected == id) selected = (sessions.lastOrNull { !it.state.terminal } ?: sessions.lastOrNull())?.id
        if (sessions.isEmpty()) visible = false
    }
    fun selectNewRemote() { selectingNewRemote = true; home() }
    fun clearStopped(ids: List<UUID>) {
        ids.distinct().forEach { id ->
            val session = sessions.firstOrNull { it.id == id } ?: return@forEach
            session.state = session.connection.state()
            if (session.state.terminal) close(id)
        }
    }
    fun cancelNewRemote() { selectingNewRemote = false }
    /** A stopped viewport stays available; recovery always creates a separate owner. */
    fun restartLocal(id: UUID) {
        val old = sessions.firstOrNull { it.id == id && it.remoteAlias == null } ?: return
        old.state = old.connection.state()
        if (old.state.terminal) newLocal()
    }
    fun reconnect(id: UUID, target: PreparedSSH) {
        if (connecting != null) return
        val old = sessions.firstOrNull { it.id == id } ?: return
        if (old.backgroundSession != null || !old.remoteAlias.equals(target.alias, ignoreCase = true)) return
        old.state = old.connection.state()
        if (!old.state.terminal) return
        selectingNewRemote = false; choosingRemote = null
        val attempt = SSHAttempt(target); connecting = attempt
        protectInBackground()
        if (target.user.isNotBlank()) startConnection(attempt)
    }
    fun connect(target: PreparedSSH) {
        if (connecting != null) return
        val existing = sessions.filter { it.remoteAlias == target.alias && it.state.connected && it.backgroundSession==null }
        if (!selectingNewRemote && existing.isNotEmpty()) {
            if (existing.size == 1) show(existing.single().id) else choosingRemote = target.alias
            return
        }
        selectingNewRemote = false; choosingRemote = null
        val attempt = SSHAttempt(target); connecting = attempt
        protectInBackground()
        if (target.user.isNotBlank()) startConnection(attempt)
    }
    /** File authentication uses the same policy and UI, but never creates a terminal. */
    fun connectFiles(target: PreparedSSH, ready: (RemoteFileSession, String) -> Unit) {
        if(connecting != null) return
        val attempt = SSHAttempt(target,ready); connecting = attempt
        if(target.user.isNotBlank()) startConnection(attempt)
    }
    fun connectTmux(target: PreparedSSH,ready: (RemoteTmuxSession,String)->Unit): UUID? {
        if(connecting!=null)return null
        val attempt=SSHAttempt(target).also { it.tmuxReady=ready }
        connecting=attempt
        if(target.user.isNotBlank())startConnection(attempt)
        return attempt.id
    }
    fun attachTmux(target: PreparedSSH,selection: RemoteTmuxTarget,name: String): Boolean {
        if(connecting!=null)return false
        val attempt=SSHAttempt(target).also { it.tmuxTarget=selection;it.tmuxName=name;it.message="正在连接后台会话客户端…" }
        connecting=attempt
        protectInBackground()
        if(target.user.isNotBlank())startConnection(attempt)
        return true
    }
    /** The task owns these sessions. Authentication only lends the existing credential UI;
     * it never adopts the session into the browser or terminal collection. */
    fun fileTaskConnections(request: FilePasteRequest, cancelled: () -> Unit = {}): FileTaskConnections {
        return fileConnections(request.id,request.source.endpoint,request.destination.endpoint,"复制任务",cancelled)
    }
    /** Fresh destination-only owner; history never becomes an executable paste request. */
    fun fileHistoryConnections(id: String, destination: FileEndpoint, cancelled: () -> Unit): FileTaskConnections =
        fileConnections(id,FileEndpoint.APP,destination,"核对历史位置",cancelled)
    private fun fileConnections(id: String, source: FileEndpoint, destination: FileEndpoint,
        label: String, cancelled: () -> Unit): FileTaskConnections {
        val targets = mutableMapOf<FileTaskConnections.Role, PreparedSSH>()
        return FileTaskConnections(source, destination,
            resolve = { role, endpoint ->
                val deadline = SystemClock.elapsedRealtime() + connectionBudgetMillis
                withTimeout(connectionBudgetMillis) {
                    val identity = checkNotNull(endpoint.remote)
                    val target = withContext(Dispatchers.IO) {
                        val repository = ConfigRepository(File(getApplication<Application>().filesDir, "home/.ssh"))
                        val snapshot = repository.load()
                        check(snapshot.hosts.any { it.alias == identity.alias }) { "来源主机配置已移除，请重新选择位置" }
                        val issues = repository.connectionIssues(snapshot, identity.alias)
                        check(issues.isEmpty()) { "不支持 SSH 配置：" + issues.joinToString("、") { it.description } }
                        val prepared = repository.prepareConnection(snapshot, identity.alias)
                        check(prepared.host == identity.host && prepared.port == identity.port &&
                            (prepared.user.isBlank() || prepared.user == identity.user)) { "主机地址或用户已改变，请重新选择位置" }
                        prepared.copy(user = identity.user, identityFiles = prepared.identityFiles.toList())
                    }
                    val addresses = withContext(Dispatchers.IO) { resolver.resolve(target.host) }
                    ensureActive()
                    check(addresses.isNotEmpty()) { "没有可用的主机地址" }
                    val known = withContext(Dispatchers.IO) {
                        File(getApplication<Application>().filesDir, "home/.ssh/known_hosts").also {
                            check(it.parentFile!!.isDirectory || it.parentFile!!.mkdirs()) { "无法准备主机信任目录" }
                        }
                    }
                    val remaining = deadline - SystemClock.elapsedRealtime()
                    check(remaining > 0) { "任务连接超时" }
                    targets[role] = target
                    ResolvedSSHConnection(target.host,target.port,target.user,known.absolutePath,
                        addresses.map { ResolvedSSHAddress(it.address,(it as? Inet6Address)?.scopeId ?: 0) },
                        remaining.coerceAtMost(60_000).toInt())
                }
            }, authenticate = { role, endpoint, connection ->
                authenticateFileTask(id,role,endpoint,targets.getValue(role),connection,label,cancelled)
            })
    }
    private suspend fun authenticateFileTask(requestId: String, role: FileTaskConnections.Role,
        endpoint: FileEndpoint, target: PreparedSSH, connection: RemoteFileSession, label: String, cancelled: () -> Unit) = withContext(Dispatchers.Main.immediate) {
        coroutineScope {
            check(connecting == null) { "另一个连接正在等待认证，请完成后重新粘贴" }
            val identity = checkNotNull(endpoint.remote)
            check(connection.peer.host == identity.host && connection.peer.port == identity.port && connection.peer.user == identity.user) {
                "任务认证连接身份不匹配"
            }
            val attempt = SSHAttempt(target).also {
                it.taskLabel = label + " · " + if(role == FileTaskConnections.Role.SOURCE) "读取来源" else if(label=="复制任务") "写入目标" else "只读目标"
                it.taskCancelled = cancelled
                it.connection = connection; it.job = coroutineContext[Job]
            }
            // Each invocation has a fresh attempt UUID, even for the other role on the same host.
            connecting = attempt
            try {
                withTimeout(connectionBudgetMillis) {
                    var listed = false
                    while(true) {
                        ensureActive()
                        check(connecting === attempt) { "任务认证请求已失效：$requestId" }
                        val auth = connection.authentication()
                        attempt.auth = auth; attempt.message = auth.description
                        val state = connection.state()
                        if(state.ready) { rememberSuccessfulIdentity(attempt)?.let { identityMemoryWarning = it }; break }
                        check(!state.finished && auth.phase != SSHAuthPhase.FAILED && auth.phase != SSHAuthPhase.CANCELLED) {
                            if(auth.phase == SSHAuthPhase.FAILED) auth.description else "任务文件连接未完成（${state.error}）"
                        }
                        if(auth.phase == SSHAuthPhase.CREDENTIALS && !listed) {
                            try { loadIdentities(attempt) }
                            catch(_: Exception) { attempt.credentialError = "无法读取身份容器，请检查或重新导入密钥" }
                            listed = true
                        }
                        if(auth.phase==SSHAuthPhase.CREDENTIALS && listed) useConfiguredIdentity(attempt,auth)
                        delay(50)
                    }
                }
            } finally {
                attempt.credentialJob?.cancel(); attempt.connection = null
                if(connecting === attempt) connecting = null
            }
        }
    }
    fun setConnectionUsername(id: UUID, value: String) {
        val attempt = connecting?.takeIf { it.id == id && it.job == null } ?: return
        val user = value.trim()
        if (user.isBlank() || user.length > 256 || user.any { it.isWhitespace() || it.isISOControl() }) {
            attempt.credentialError = "请填写有效的远端用户名"; return
        }
        attempt.username = user; startConnection(attempt)
    }
    private fun startConnection(attempt: SSHAttempt) {
        if (!attempt.forFiles && attempt.tmuxReady == null) protectInBackground()
        attempt.job = viewModelScope.launch {
            var owned: AuthenticatedSSH? = null
            try {
                val deadline = SystemClock.elapsedRealtime() + connectionBudgetMillis
                withTimeout(connectionBudgetMillis) {
                    val addresses = if(attempt.tmuxTarget==null) withContext(Dispatchers.IO) { resolver.resolve(attempt.target.host) } else emptyList()
                    ensureActive()
                    check(connecting === attempt && (attempt.tmuxTarget!=null || addresses.isNotEmpty())) { "连接请求已取消或没有可用地址" }
                    val candidates = addresses.map { ResolvedSSHAddress(it.address, (it as? Inet6Address)?.scopeId ?: 0) }
                    val remaining = deadline - SystemClock.elapsedRealtime()
                    check(remaining > 0) { "SSH 连接超时" }
                    withContext(Dispatchers.IO) {
                        val knownHosts = File(getApplication<Application>().filesDir, "home/.ssh/known_hosts")
                        check(knownHosts.parentFile!!.isDirectory || knownHosts.parentFile!!.mkdirs()) { "无法准备主机信任目录" }
                        val request = attempt.tmuxTarget?.connection(remaining.coerceAtMost(60_000).toInt()) ?: ResolvedSSHConnection(attempt.target.host, attempt.target.port, attempt.username,
                            knownHosts.absolutePath, candidates, remaining.coerceAtMost(60_000).toInt())
                        owned = if(attempt.tmuxReady!=null) RemoteTmuxSession(request)
                            else if(attempt.forFiles) RemoteFileSession(request)
                            else attempt.tmuxTarget?.let { RemoteSession(request,it) } ?: RemoteSession(request)
                    }
                    ensureActive()
                    check(connecting === attempt) { "连接请求已取消" }
                    attempt.connection = owned
                    var listed = false
                    while (true) {
                        ensureActive()
                        val connection = checkNotNull(owned)
                        val auth = connection.authentication()
                        // Configured keys authenticate without briefly presenting a credential form.
                        // A rejected request or unlock error returns to explicit user input.
                        attempt.automaticFileAuthentication = attempt.filesReady != null &&
                            auth.phase == SSHAuthPhase.CREDENTIALS && !attempt.manualCredentials &&
                            attempt.credentialError == null && (!listed || attempt.credentialBusy ||
                            auth.request in attempt.automaticKeyRequests ||
                            (auth.methods and 1 != 0 && com.hanxi.ghosterm.data.IdentityReferences.automaticCandidate(
                                attempt.authenticationIdentityFiles,attempt.identities,attempt.triedIdentityIds) != null))
                        attempt.auth = auth; attempt.message = auth.description
                        if(attempt.forFiles && auth.phase in listOf(SSHAuthPhase.READY,SSHAuthPhase.TAKEN))
                            attempt.message = "正在打开远端文件…"
                        val terminalState = (connection as? RemoteSession)?.state()
                        val fileState = (connection as? RemoteFileSession)?.state()
                        val tmuxState=(connection as? RemoteTmuxSession)?.state()
                        if(attempt.tmuxReady!=null && auth.phase in listOf(SSHAuthPhase.READY,SSHAuthPhase.TAKEN))
                            attempt.message="正在准备后台会话管理…"
                        if (terminalState?.connected == true || fileState?.ready == true || tmuxState?.ready==true) {
                            val identityWarning = rememberSuccessfulIdentity(attempt)
                            if(connection is RemoteTmuxSession) {
                                checkNotNull(attempt.tmuxReady)(connection,attempt.username)
                                owned=null;attempt.connection=null;connecting=null
                            } else if(connection is RemoteFileSession) {
                                checkNotNull(attempt.filesReady)(connection,attempt.username)
                                owned = null; attempt.connection = null; connecting = null
                            } else {
                                val session = TerminalSession(UUID.randomUUID(),
                                    attempt.target.alias+(attempt.tmuxName?.let { " · "+it } ?: ""),
                                    connection as RemoteSession, attempt.target.alias,attempt.tmuxName,
                                    attempt.target.copy(user=attempt.username,identityFiles=(attempt.submittedIdentityId?.let {
                                        listOf(com.hanxi.ghosterm.data.IdentityReferences.encode(it))
                                    } ?: attempt.target.identityFiles).toList()))
                                session.state = checkNotNull(terminalState)
                                register(session); owned = null; attempt.connection = null; connecting = null; show(session.id)
                            }
                            identityWarning?.let { identityMemoryWarning = it }
                            break
                        }
                        if (terminalState?.terminal == true || fileState?.finished == true || tmuxState?.finished==true || auth.phase == SSHAuthPhase.FAILED || auth.phase == SSHAuthPhase.CANCELLED) {
                            attempt.message = if (auth.phase == SSHAuthPhase.FAILED) auth.description else
                                terminalState?.description ?: if(tmuxState!=null) "后台连接失败（错误 "+tmuxState.error+"）"
                                else "无法打开远端文件（错误 ${fileState?.error}）"
                            attempt.failed = true; break
                        }
                        if (auth.phase == SSHAuthPhase.CREDENTIALS && !listed) {
                            try { loadIdentities(attempt) }
                            catch (_: Exception) { attempt.credentialError = "无法读取身份容器，请检查或重新导入密钥" }
                            listed = true
                        }
                        if(auth.phase==SSHAuthPhase.CREDENTIALS && listed) useConfiguredIdentity(attempt,auth)
                        delay(50)
                    }
                }
            } catch (_: TimeoutCancellationException) {
                if (connecting === attempt) { attempt.message = "SSH 连接超时"; attempt.failed = true }
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) {
                if (connecting === attempt) { attempt.message = failure.message ?: "SSH 连接失败"; attempt.failed = true }
            } finally { attempt.credentialJob?.cancel(); owned?.close(); attempt.connection = null }
        }
    }
    fun cancelConnection(id: UUID) {
        val attempt = connecting?.takeIf { it.id == id } ?: return
        connecting = null; attempt.taskCancelled?.invoke(); attempt.credentialJob?.cancel(); attempt.job?.cancel(); attempt.connection?.close(); attempt.connection = null
    }
    private fun current(id: UUID, prompt: SSHAuthState): SSHAttempt {
        val attempt = checkNotNull(connecting?.takeIf { it.id == id && !it.failed }) { "连接请求已失效" }
        check(prompt.request > 0 && attempt.auth?.request == prompt.request && attempt.auth?.phase == prompt.phase) { "认证请求已失效" }
        return attempt
    }
    fun decide(id: UUID, prompt: SSHAuthState, accept: Boolean) {
        try { current(id,prompt).connection!!.decide(prompt,accept) }
        catch (_: Exception) { connecting?.takeIf { it.id == id }?.credentialError = "主机确认已失效，请等待当前连接状态" }
    }
    fun manualAuthentication(id: UUID) { connecting?.takeIf { it.id==id }?.manualCredentials=true }
    private suspend fun loadIdentities(attempt: SSHAttempt) {
        val (identities, remembered) = withContext(Dispatchers.IO) {
            val identities = vault.list()
            identities to rememberedIdentities.find(attempt.target, attempt.username, identities)
        }
        attempt.identities = identities
        attempt.rememberedIdentityId = remembered?.id
    }
    private suspend fun rememberSuccessfulIdentity(attempt: SSHAttempt): String? {
        val id = attempt.submittedIdentityId ?: return null
        if (!attempt.manuallySubmittedIdentity) return null
        return try {
            withContext(Dispatchers.IO) {
                ensureActive()
                rememberedIdentities.record(attempt.target, attempt.username, id, attempt.rememberIdentity)
            }
            null
        } catch (cancel: CancellationException) { throw cancel }
        catch (_: Exception) { "SSH 已连接，但未能记住密钥；下次连接可能仍需选择" }
    }
    private fun useConfiguredIdentity(attempt: SSHAttempt, prompt: SSHAuthState) {
        if(attempt.manualCredentials || attempt.credentialBusy || prompt.request<=0 || prompt.methods and 1==0 ||
            prompt.request in attempt.automaticKeyRequests) return
        val candidate=com.hanxi.ghosterm.data.IdentityReferences.automaticCandidate(
            attempt.authenticationIdentityFiles,attempt.identities,attempt.triedIdentityIds) ?: return
        attempt.automaticKeyRequests.add(prompt.request)
        useIdentity(attempt.id,prompt,candidate,ByteArray(0),automatic=true)
    }
    fun password(id: UUID, prompt: SSHAuthState, secret: ByteArray) {
        try { current(id,prompt).let { it.manualCredentials=true;it.submittedIdentityId=null;it.manuallySubmittedIdentity=false;it.connection!!.password(prompt,secret) } }
        catch (_: Exception) { connecting?.takeIf { it.id == id }?.credentialError = "凭据请求已失效或不支持此方式" }
        finally { secret.fill(0) }
    }
    fun useIdentity(id: UUID, prompt: SSHAuthState, identity: StoredIdentity, passphrase: ByteArray, automatic: Boolean = false) {
        val attempt = try { current(id,prompt) } catch (_: Exception) { passphrase.fill(0); return }
        if (attempt.credentialBusy) { passphrase.fill(0); return }
        if(!automatic) attempt.manualCredentials=true
        attempt.triedIdentityIds.add(identity.id)
        attempt.credentialBusy = true; attempt.credentialError = null
        attempt.credentialJob = viewModelScope.launch {
            var material: ByteArray? = null
            try {
                withContext(Dispatchers.IO) { material = vault.unlock(identity) }
                current(id,prompt).connection!!.privateKey(prompt,checkNotNull(material),passphrase)
                attempt.submittedIdentityId=identity.id
                attempt.manuallySubmittedIdentity=!automatic
            } catch (cancel: CancellationException) { throw cancel }
            catch (_: Exception) { if (connecting === attempt) attempt.credentialError = "无法解锁或使用此身份，请重新选择或导入" }
            finally { material?.fill(0); passphrase.fill(0); attempt.credentialBusy = false }
        }
    }
    fun beginIdentityImport(id: UUID, prompt: SSHAuthState, presenter: UUID? = null): Boolean {
        if (identityPickerPending) return false
        val attempt = try { current(id,prompt) } catch (_: Exception) { return false }
        if (attempt.credentialBusy || prompt.phase != SSHAuthPhase.CREDENTIALS) return false
        pendingIdentityImport = id to prompt
        pendingIdentityPresenter = presenter
        identityPickerPending = true
        return true
    }
    fun identityImportLaunchFailed(presenter: UUID? = null) {
        if(pendingIdentityPresenter != presenter) return
        pendingIdentityImport?.first?.let { id ->
            connecting?.takeIf { it.id == id }?.credentialError = "无法打开系统文件选择器"
        }
        pendingIdentityImport = null; pendingIdentityPresenter = null; identityPickerPending = false
    }
    fun completeIdentityImport(uri: Uri?, presenter: UUID? = null) {
        if(pendingIdentityPresenter != presenter) return
        val pending = pendingIdentityImport
        pendingIdentityImport = null; pendingIdentityPresenter = null; identityPickerPending = false
        if (pending != null && uri != null) importIdentity(pending.first,pending.second,uri)
    }
    fun releaseIdentityPresenter(presenter: UUID) {
        if(pendingIdentityPresenter != presenter) return
        pendingIdentityImport?.first?.let { id ->
            connecting?.takeIf { it.id == id }?.credentialError = "原页面已关闭，请重新选择密钥"
        }
        pendingIdentityImport = null; pendingIdentityPresenter = null; identityPickerPending = false
    }
    private fun importIdentity(id: UUID, prompt: SSHAuthState, uri: Uri) {
        val attempt = try { current(id,prompt) } catch (_: Exception) { return }
        if (attempt.credentialBusy) return
        attempt.credentialBusy = true; attempt.credentialError = null
        attempt.credentialJob = viewModelScope.launch {
            var material: ByteArray? = null
            try {
                val document = IdentityDocumentReader.read(getApplication<Application>().contentResolver,uri)
                material = document.bytes
                current(id,prompt)
                withContext(Dispatchers.IO) {
                    ensureActive()
                    // Once the atomic vault transaction begins it completes or reports its commit state.
                    vault.import(document.name,document.bytes)
                }
                if (connecting === attempt) attempt.identities = withContext(Dispatchers.IO) { vault.list() }
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) { if (connecting === attempt) attempt.credentialError = failure.message ?: "密钥导入失败" }
            finally { material?.fill(0); attempt.credentialBusy = false }
        }
    }
    override fun onCleared() {
        connecting?.let { cancelConnection(it.id) }
        background?.detached(this)
        if (!processTerminals) sessions.forEach { it.connection.close() }
        super.onCleared()
    }
}

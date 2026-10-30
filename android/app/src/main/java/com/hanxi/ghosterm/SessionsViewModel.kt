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
import android.provider.OpenableColumns
import java.net.Inet6Address
import kotlinx.coroutines.Job
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.ensureActive
import com.hanxi.ghosterm.terminal.TerminalSurface
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

class TerminalSession(val id: UUID, val title: String, internal val connection: NativeTerminalSession, val remoteAlias: String? = null) {
    val surface: TerminalSurface get() = connection
    var state by mutableStateOf(SessionState(0, null, 0, null)); internal set
}

/** Activity ViewModel lifetime survives rotation; navigating never closes a session. */
class SSHAttempt(val target: PreparedSSH) {
    val id: UUID = UUID.randomUUID()
    var username by mutableStateOf(target.user)
    var message by mutableStateOf("正在解析主机地址…")
    var failed by mutableStateOf(false)
    var auth by mutableStateOf<SSHAuthState?>(null)
    var identities by mutableStateOf<List<StoredIdentity>>(emptyList())
    var credentialBusy by mutableStateOf(false)
    var credentialError by mutableStateOf<String?>(null)
    internal var connection: RemoteSession? = null
    internal var job: Job? = null
}

class SessionsViewModel @JvmOverloads constructor(application: Application, private val resolver: SSHResolver = AndroidSSHResolver,
    private val connectionBudgetMillis: Long = 60_000) : AndroidViewModel(application) {
    val sessions = mutableStateListOf<TerminalSession>()
    var selected by mutableStateOf<UUID?>(null); private set
    var visible by mutableStateOf(false); private set
    var creating by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    private var sequence = 0
    var connecting by mutableStateOf<SSHAttempt?>(null); private set
    var selectingNewRemote by mutableStateOf(false); private set
    var choosingRemote by mutableStateOf<String?>(null); private set
    fun dismissRemoteChoices() { choosingRemote = null }
    private val vault = IdentityVault(File(application.filesDir, "identities"))
    val active: TerminalSession? get() = sessions.firstOrNull { it.id == selected }
    init {
        viewModelScope.launch {
            while (true) {
                sessions.forEach { it.state = it.connection.state() }
                delay(250)
            }
        }
    }
    fun local() {
        val previous = active?.takeIf { it.remoteAlias == null } ?: sessions.lastOrNull { it.remoteAlias == null }
        if (previous == null) newLocal() else show(previous.id)
    }
    fun newLocal() {
        if (creating) return
        creating = true; error = null
        viewModelScope.launch {
            var created: LocalSession? = null
            try {
                withContext(Dispatchers.IO) {
                    val home = File(getApplication<Application>().filesDir, "home")
                    check(home.isDirectory || home.mkdirs()) { "无法创建本地终端目录" }
                    created = LocalSession(home.absolutePath)
                }
                val session = TerminalSession(UUID.randomUUID(), "本地终端 ${++sequence}", checkNotNull(created))
                sessions.add(session); created = null
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
    fun close(id: UUID) {
        val session = sessions.firstOrNull { it.id == id } ?: return
        session.connection.close(); sessions.remove(session)
        if (selected == id) selected = sessions.lastOrNull()?.id
        if (sessions.isEmpty()) visible = false
    }
    fun selectNewRemote() { selectingNewRemote = true; home() }
    fun cancelNewRemote() { selectingNewRemote = false }
    fun connect(target: PreparedSSH) {
        if (connecting != null) return
        val existing = sessions.filter { it.remoteAlias == target.alias && it.state.connected }
        if (!selectingNewRemote && existing.isNotEmpty()) {
            if (existing.size == 1) show(existing.single().id) else choosingRemote = target.alias
            return
        }
        selectingNewRemote = false; choosingRemote = null
        val attempt = SSHAttempt(target); connecting = attempt
        if (target.user.isNotBlank()) startConnection(attempt)
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
        attempt.job = viewModelScope.launch {
            var owned: RemoteSession? = null
            try {
                val deadline = SystemClock.elapsedRealtime() + connectionBudgetMillis
                withTimeout(connectionBudgetMillis) {
                    val addresses = withContext(Dispatchers.IO) { resolver.resolve(attempt.target.host) }
                    ensureActive()
                    check(connecting === attempt && addresses.isNotEmpty()) { "连接请求已取消或没有可用地址" }
                    val address = addresses.first()
                    val remaining = deadline - SystemClock.elapsedRealtime()
                    check(remaining > 0) { "SSH 连接超时" }
                    withContext(Dispatchers.IO) {
                        val knownHosts = File(getApplication<Application>().filesDir, "home/.ssh/known_hosts")
                        check(knownHosts.parentFile!!.isDirectory || knownHosts.parentFile!!.mkdirs()) { "无法准备主机信任目录" }
                        owned = RemoteSession(ResolvedSSHConnection(attempt.target.host, attempt.target.port, attempt.username,
                            knownHosts.absolutePath, address.address, (address as? Inet6Address)?.scopeId ?: 0, remaining.coerceAtMost(60_000).toInt()))
                    }
                    ensureActive()
                    check(connecting === attempt) { "连接请求已取消" }
                    attempt.connection = owned
                    var listed = false
                    while (true) {
                        ensureActive()
                        val connection = checkNotNull(owned)
                        val auth = connection.authentication(); attempt.auth = auth; attempt.message = auth.description
                        val state = connection.state()
                        if (state.connected) {
                            val session = TerminalSession(UUID.randomUUID(), attempt.target.alias, connection, attempt.target.alias)
                            session.state = state
                            sessions.add(session); owned = null; attempt.connection = null; connecting = null; show(session.id)
                            break
                        }
                        if (state.terminal || auth.phase == SSHAuthPhase.FAILED || auth.phase == SSHAuthPhase.CANCELLED) {
                            attempt.message = if (auth.phase == SSHAuthPhase.FAILED) auth.description else state.description
                            attempt.failed = true; break
                        }
                        if (auth.phase == SSHAuthPhase.CREDENTIALS && !listed) {
                            try { attempt.identities = withContext(Dispatchers.IO) { vault.list() } }
                            catch (_: Exception) { attempt.credentialError = "无法读取身份容器，请检查或重新导入密钥" }
                            listed = true
                        }
                        delay(50)
                    }
                }
            } catch (_: TimeoutCancellationException) {
                if (connecting === attempt) { attempt.message = "SSH 连接超时"; attempt.failed = true }
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) {
                if (connecting === attempt) { attempt.message = failure.message ?: "SSH 连接失败"; attempt.failed = true }
            } finally { owned?.close(); attempt.connection = null }
        }
    }
    fun cancelConnection(id: UUID) {
        val attempt = connecting?.takeIf { it.id == id } ?: return
        connecting = null; attempt.job?.cancel(); attempt.connection?.close(); attempt.connection = null
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
    fun password(id: UUID, prompt: SSHAuthState, secret: ByteArray) {
        try { current(id,prompt).connection!!.password(prompt,secret) }
        catch (_: Exception) { connecting?.takeIf { it.id == id }?.credentialError = "凭据请求已失效或不支持此方式" }
        finally { secret.fill(0) }
    }
    fun useIdentity(id: UUID, prompt: SSHAuthState, identity: StoredIdentity, passphrase: ByteArray) {
        val attempt = try { current(id,prompt) } catch (_: Exception) { passphrase.fill(0); return }
        if (attempt.credentialBusy) { passphrase.fill(0); return }
        attempt.credentialBusy = true; attempt.credentialError = null
        viewModelScope.launch {
            var material: ByteArray? = null
            try {
                withContext(Dispatchers.IO) { material = vault.unlock(identity) }
                current(id,prompt).connection!!.privateKey(prompt,checkNotNull(material),passphrase)
            } catch (cancel: CancellationException) { throw cancel }
            catch (_: Exception) { if (connecting === attempt) attempt.credentialError = "无法解锁或使用此身份，请重新选择或导入" }
            finally { material?.fill(0); passphrase.fill(0); attempt.credentialBusy = false }
        }
    }
    fun importIdentity(id: UUID, prompt: SSHAuthState, uri: Uri) {
        val attempt = try { current(id,prompt) } catch (_: Exception) { return }
        if (attempt.credentialBusy) return
        attempt.credentialBusy = true; attempt.credentialError = null
        viewModelScope.launch {
            val buffer = ByteArray(1024*1024+1)
            var material: ByteArray? = null
            try {
                withContext(Dispatchers.IO) {
                    val resolver = getApplication<Application>().contentResolver
                    val name = resolver.query(uri,arrayOf(OpenableColumns.DISPLAY_NAME),null,null,null)?.use {
                        if (it.moveToFirst()) it.getString(0) else null
                    } ?: "导入的 SSH 密钥"
                    var count = 0
                    checkNotNull(resolver.openInputStream(uri)) { "无法读取所选密钥" }.use { input ->
                        while (count < buffer.size) { val n = input.read(buffer,count,buffer.size-count); if (n < 0) break; if (n == 0) continue; count += n }
                    }
                    check(count in 1..1024*1024) { "密钥必须为 1 字节至 1 MiB" }
                    material = buffer.copyOf(count)
                    vault.import(name,checkNotNull(material))
                }
                if (connecting === attempt) attempt.identities = withContext(Dispatchers.IO) { vault.list() }
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) { if (connecting === attempt) attempt.credentialError = failure.message ?: "密钥导入失败" }
            finally { buffer.fill(0); material?.fill(0); attempt.credentialBusy = false }
        }
    }
    override fun onCleared() { connecting?.let { cancelConnection(it.id) }; sessions.forEach { it.connection.close() }; super.onCleared() }
}

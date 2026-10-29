package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.bridge.SessionState
import com.hanxi.ghosterm.terminal.TerminalSurface
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

class TerminalSession(val id: UUID, val title: String, internal val connection: LocalSession) {
    val surface: TerminalSurface get() = connection
    var state by mutableStateOf(SessionState(0, null, 0, null)); internal set
}

/** Activity ViewModel lifetime survives rotation; navigating never closes a session. */
class SessionsViewModel(application: Application) : AndroidViewModel(application) {
    val sessions = mutableStateListOf<TerminalSession>()
    var selected by mutableStateOf<UUID?>(null); private set
    var visible by mutableStateOf(false); private set
    var creating by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    private var sequence = 0
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
        val previous = active ?: sessions.lastOrNull()
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
    fun show(id: UUID) { if (sessions.any { it.id == id }) { selected = id; visible = true; error = null } }
    fun home() { visible = false }
    fun reportError(message: String) { error = message }
    fun dismissError() { error = null }
    fun close(id: UUID) {
        val session = sessions.firstOrNull { it.id == id } ?: return
        session.connection.close(); sessions.remove(session)
        if (selected == id) selected = sessions.lastOrNull()?.id
        if (sessions.isEmpty()) visible = false
    }
    override fun onCleared() { sessions.forEach { it.connection.close() }; super.onCleared() }
}

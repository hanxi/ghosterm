package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import java.util.UUID

enum class SettingsSection(val title: String, val subtitle: String) {
    APPEARANCE("外观与终端", "配色、字号、行距与光标"), INPUT("输入与键盘", "辅助键、Ctrl 和键盘唤起"),
    SESSIONS("会话与连接", "屏幕常亮与连接资料"), FILES("文件操作", "隐藏文件、排序与粘贴习惯"),
    SECURITY("身份与主机验证", "密钥信息与已验证主机")
}

/** Candidate edits are local; only the app owner's confirmed snapshot applies globally. */
class SettingsViewModel @JvmOverloads constructor(application: Application,
    val settings: AppSettings = AppSettings.forApplication(application)) : AndroidViewModel(application) {
    private val client = UUID.randomUUID()
    private data class Pending(val edit: PreferenceEdit, val serial: Long)
    private val edits = linkedMapOf<PreferenceKey, Pending>()
    private var serial = 0L
    private var attempt = 0L
    var storage by mutableStateOf<PreferencesState>(settings.state.value.storage); private set
    var candidate by mutableStateOf(settings.state.value.committed); private set
    var section by mutableStateOf<SettingsSection?>(null); private set
    var inFlight by mutableIntStateOf(0); private set
    var reloading by mutableStateOf(false); private set
    var unsaved by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var resetConfirmation by mutableStateOf(false); private set
    var reloadConfirmation by mutableStateOf(false); private set
    var leaveConfirmation by mutableStateOf(false); private set
    var leaveRequested by mutableStateOf(false); private set
    val canEdit get() = storage is PreferencesState.Ready && !reloading
    init {
        viewModelScope.launch {
            settings.state.collect { state ->
                if (state !== settings.state.value) return@collect
                storage = state.storage
                refreshCandidate()
            }
        }
    }
    private fun refreshCandidate() {
        candidate = edits.values.fold(settings.state.value.committed) { p, pending -> pending.edit.apply(p) }
        unsaved = edits.isNotEmpty()
    }
    fun open(section: SettingsSection) { this.section = section }
    fun back(): Boolean {
        if (section != null) { section = null; return true }
        if (unsaved && inFlight == 0) { leaveConfirmation = true; return true }
        return false
    }
    fun consumeLeave(): Boolean = leaveRequested.also { leaveRequested = false }
    fun keepEditing() { leaveConfirmation = false }
    fun discardAndLeave() {
        if (!leaveConfirmation || inFlight != 0) return
        discard(); leaveConfirmation = false; leaveRequested = true
    }
    fun edit(edit: PreferenceEdit) {
        if (!canEdit) return
        try { edit.apply(candidate) } catch (_: IllegalArgumentException) { error = "设置值无效，尚未保存。"; return }
        resetConfirmation = false; reloadConfirmation = false; leaveConfirmation = false
        serial++; edits[edit.key] = Pending(edit, serial); refreshCandidate()
        submit(reset = false)
    }
    private fun submit(reset: Boolean) {
        val captured = edits.toMap()
        val ticket = ++attempt
        inFlight++
        viewModelScope.launch {
            try {
                when (val result = settings.save(client, captured.values.map { it.edit }, reset)) {
                    is PreferenceUpdate.Saved -> {
                        captured.forEach { (key, value) -> if (edits[key]?.serial == value.serial) edits.remove(key) }
                        if (ticket == attempt) error = null
                    }
                    is PreferenceUpdate.Failed -> if (ticket == attempt) error = result.message
                }
                storage = settings.state.value.storage; refreshCandidate()
            } catch (cancelled: CancellationException) { throw cancelled }
            finally { inFlight-- }
        }
    }
    fun retry() { if (canEdit && unsaved && inFlight == 0) submit(reset = false) }
    fun discard() {
        if (inFlight != 0) return
        edits.clear(); error = null; refreshCandidate()
    }
    fun requestReset() { if (canEdit && inFlight == 0) resetConfirmation = true }
    fun cancelReset() { resetConfirmation = false }
    fun confirmReset() {
        if (!resetConfirmation || !canEdit || inFlight != 0) return
        resetConfirmation = false; serial++
        edits.clear(); PreferenceEdit.defaults().forEach { edits[it.key] = Pending(it, serial) }
        refreshCandidate(); submit(reset = true)
    }
    fun requestReload() {
        if (inFlight != 0) return
        if (unsaved) reloadConfirmation = true else reload()
    }
    fun cancelReload() { reloadConfirmation = false }
    fun confirmReload() {
        if (!reloadConfirmation || inFlight != 0) return
        reloadConfirmation = false; reload()
    }
    private fun reload() {
        reloading = true; inFlight++
        viewModelScope.launch {
            try {
                when (val result = settings.reload(client)) {
                    is SettingsReload.Loaded -> if (result.state is PreferencesState.Ready) {
                        edits.clear(); error = null; storage = settings.state.value.storage; refreshCandidate()
                    } else error = (result.state as? PreferencesState.Unavailable)?.message
                    is SettingsReload.Rejected -> error = result.message
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            finally { reloading = false; inFlight-- }
        }
    }
}

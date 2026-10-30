package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.KnownHostsRepository
import com.hanxi.ghosterm.data.KnownHostsSnapshot
import kotlinx.coroutines.*
import java.io.File

class KnownHostsViewModel(application: Application,
    private val repository: KnownHostsRepository) : AndroidViewModel(application) {
    constructor(application: Application) : this(application, KnownHostsRepository(File(application.filesDir, "home/.ssh")))
    var snapshot by mutableStateOf<KnownHostsSnapshot?>(null); private set
    private var selectedId by mutableStateOf<String?>(null)
    val selected get() = snapshot?.records?.find { it.id == selectedId }
    var loading by mutableStateOf(false); private set
    var writing by mutableStateOf(false); private set
    val busy get() = loading || writing
    var error by mutableStateOf<String?>(null); private set
    var notice by mutableStateOf<String?>(null); private set
    var confirmingRemoval by mutableStateOf(false); private set
    private var present = false
    private var generation = 0L
    private var readJob: Job? = null
    private fun cancelRead() { generation++; readJob?.cancel(); readJob = null; loading = false }
    fun enter() { present = true; if (!writing) load() }
    fun leave() {
        present = false; cancelRead(); snapshot = null; selectedId = null; confirmingRemoval = false; error = null
        // An already confirmed native write is not cancelled by page navigation.
    }
    fun reload() { if (!writing) { notice = null; load() } }
    private fun load() {
        if (!present) return
        cancelRead(); snapshot = null; selectedId = null; confirmingRemoval = false; error = null; loading = true
        val token = generation
        readJob = viewModelScope.launch {
            try {
                val next = withContext(Dispatchers.IO) { repository.load() }
                if (present && generation == token) snapshot = next
            } catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) {
                if (present && generation == token) error = "无法读取主机验证记录：${failure.message ?: "请重试"}"
            } finally { if (generation == token) loading = false }
        }
    }
    fun open(id: String) {
        if (!busy && snapshot?.records?.any { it.id == id } == true) { selectedId = id; confirmingRemoval = false }
    }
    fun backToList(): Boolean {
        if (confirmingRemoval) { confirmingRemoval = false; return true }
        if (selectedId == null) return false
        selectedId = null; return true
    }
    fun requestRemoval() { if (!busy && selected?.removable == true) confirmingRemoval = true }
    fun cancelRemoval() { confirmingRemoval = false }
    fun remove() {
        val before = snapshot ?: return
        val entry = selected ?: return
        if (!confirmingRemoval || busy || !entry.removable) return
        confirmingRemoval = false; writing = true; notice = null; error = null
        viewModelScope.launch {
            try {
                val result = withContext(Dispatchers.IO) { repository.remove(before, entry.id) }
                notice = when {
                    result.succeeded -> "记录已移除"
                    result.committed -> "记录修改已提交，但后续核验失败；请核对重新载入的记录。${result.message}"
                    result.conflict -> "记录已变化，本次移除未提交；请重新选择并核对。"
                    else -> "本次移除未提交：${result.message}"
                }
            } catch (cancel: CancellationException) { throw cancel }
            catch (_: Exception) { notice = "移除结果无法确认，请重新载入并核对记录。" }
            finally {
                writing = false; snapshot = null; selectedId = null
                if (present) load()
            }
        }
    }
}

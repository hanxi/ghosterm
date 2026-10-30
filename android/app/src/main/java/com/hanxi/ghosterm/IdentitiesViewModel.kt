package com.hanxi.ghosterm

import android.app.Application
import android.net.Uri
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.*
import kotlinx.coroutines.*
import java.io.File

/** Settings owns its own import request; it never reuses an SSH authentication request. */
class IdentitiesViewModel(application: Application) : AndroidViewModel(application) {
    private val vault = IdentityVault(File(application.filesDir,"identities"))
    private val repository = ConfigRepository(File(application.filesDir,"home/.ssh"))
    var usages by mutableStateOf<List<IdentityUsage>>(emptyList()); private set
    var usageError by mutableStateOf<String?>(null); private set
    var records by mutableStateOf<List<IdentityRecord>>(emptyList()); private set
    var selected by mutableStateOf<IdentityRecord?>(null); private set
    var info by mutableStateOf<IdentityPublicInfo?>(null); private set
    var busy by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var notice by mutableStateOf<String?>(null); private set
    var confirmingRemoval by mutableStateOf(false); private set
    var pickerPending by mutableStateOf(false); private set
    private var present = false
    private var generation = 0L
    private var pickerGeneration: Long? = null
    private var job: Job? = null
    fun enter() { present = true; reload() }
    fun leave() { present = false; invalidate(); selected = null; info = null; usages = emptyList(); usageError = null; confirmingRemoval = false }
    private fun invalidate() {
        generation++; job?.cancel(); job = null; busy = false
        pickerGeneration = null; pickerPending = false
    }
    private fun work(action: suspend (Long) -> Unit) {
        if (busy || !present) return
        val token = generation
        busy = true; error = null; notice = null
        job = viewModelScope.launch {
            try { action(token) }
            catch (cancel: CancellationException) { throw cancel }
            catch (failure: Exception) { if (present && generation == token) error = failure.message ?: "身份操作失败，请重试" }
            finally { if (generation == token) busy = false }
        }
    }
    private fun current(token: Long) = present && generation == token
    fun reload() {
        invalidate(); records = emptyList(); selected = null; info = null; confirmingRemoval = false
        usages = emptyList(); usageError = null
        work { token ->
            val loaded = withContext(Dispatchers.IO) { vault.inventory() }
            if (current(token)) records = loaded
        }
    }
    fun open(record: IdentityRecord) {
        if (busy || pickerPending || record !in records) return
        selected = record; info = null; error = record.problem; notice = null
        usages = emptyList(); usageError = null
        work { token ->
            val loaded = withContext(Dispatchers.IO) {
                val publicInfo = runCatching { record.identity?.let { vault.details(record) } }
                val references = try {
                    Result.success(IdentityReferences.usages(repository.load(), record.id) { ensureActive() })
                } catch (cancel: CancellationException) { throw cancel }
                catch (failure: Exception) { Result.failure<List<IdentityUsage>>(failure) }
                publicInfo to references
            }
            if (current(token) && selected == record) {
                info = loaded.first.getOrNull()
                error = loaded.first.exceptionOrNull()?.message ?: record.problem
                usages = loaded.second.getOrDefault(emptyList())
                usageError = if (loaded.second.isFailure) "无法确定引用主机，请检查 SSH 配置后重新载入；不能视为未被使用。" else null
            }
        }
        // Public metadata damage remains visible while the independent reference query runs.
        error = record.problem
    }
    /** Detail back never leaves settings; cancelling a read invalidates late results. */
    fun backToList(): Boolean {
        if (selected == null) return false
        reload(); return true
    }
    fun beginImport(): Boolean {
        if (!present || busy || pickerPending) return false
        pickerGeneration = generation; pickerPending = true; error = null; notice = null
        return true
    }
    fun importLaunchFailed() {
        pickerGeneration = null; pickerPending = false; error = "无法打开系统文件选择器"
    }
    fun completeImport(uri: Uri?) {
        val token = pickerGeneration
        pickerGeneration = null; pickerPending = false
        if (uri == null || token == null || !current(token)) return
        work { request ->
            var material: ByteArray? = null
            try {
                val document = IdentityDocumentReader.read(getApplication<Application>().contentResolver,uri)
                material = document.bytes
                ensureCurrent(request)
                withContext(Dispatchers.IO) {
                    ensureActive()
                    vault.import(document.name,document.bytes)
                }
                val loaded = withContext(Dispatchers.IO) { vault.inventory() }
                if (current(request)) { records = loaded; notice = "身份已导入" }
            } finally { material?.fill(0) }
        }
    }
    private fun ensureCurrent(token: Long) { check(current(token)) { "导入请求已取消" } }
    fun requestRemoval() { if (!busy && selected?.revision != null) confirmingRemoval = true }
    fun cancelRemoval() { confirmingRemoval = false }
    fun remove() {
        val record = selected ?: return
        if (!confirmingRemoval || busy) return
        confirmingRemoval = false
        work { token ->
            withContext(Dispatchers.IO) { vault.removeRecord(record) }
            val loaded = withContext(Dispatchers.IO) { vault.inventory() }
            if (current(token)) { records = loaded; selected = null; info = null; usages = emptyList(); usageError = null; notice = "身份已移除" }
        }
    }
}

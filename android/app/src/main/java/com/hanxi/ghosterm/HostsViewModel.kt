package com.hanxi.ghosterm

import android.app.Application
import android.net.Uri
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

enum class Page { HOSTS, SETTINGS, ABOUT, CONFIG, EDIT_HOST }
data class HostDraft(val original: Host?, val alias: String, val hostname: String, val user: String, val port: String) {
    fun edit() = HostEdit(original?.alias, alias.trim(), hostname.trim(), user.trim(), port.trim())
    val dirty: Boolean get() = if (original == null) alias.isNotEmpty() || hostname.isNotEmpty() || user.isNotEmpty() || port != "22"
        else alias != original.alias || hostname != original.hostname || user != original.user || port != original.port.toString()
}

class HostsViewModel(application: Application) : AndroidViewModel(application) {
    private val repository = ConfigRepository(File(application.filesDir, "home/.ssh"))
    var snapshot by mutableStateOf<ConfigSnapshot?>(null); private set
    var busy by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var notice by mutableStateOf<String?>(null); private set
    var page by mutableStateOf(Page.HOSTS); private set
    var query by mutableStateOf("")
    var draft by mutableStateOf<HostDraft?>(null)
    var configText by mutableStateOf("")
    var discard by mutableStateOf(false); private set
    var imported by mutableStateOf<ByteArray?>(null); private set
    private var configReturn = Page.SETTINGS
    val filteredHosts: List<Host> get() = snapshot?.hosts.orEmpty().filter {
        query.isBlank() || it.alias.contains(query, true) || it.address.contains(query, true)
    }
    val dirty: Boolean get() = when (page) {
        Page.CONFIG -> configText != snapshot?.text
        Page.EDIT_HOST -> draft?.dirty == true
        else -> false
    }
    init { reload() }
    private fun work(action: suspend () -> Unit) {
        if (busy) return
        busy = true; error = null; notice = null
        viewModelScope.launch {
            try { action() }
            catch (failure: Exception) { error = failure.message ?: "操作失败，请重试" }
            finally { busy = false }
        }
    }
    fun reload() = work {
        val loaded = withContext(Dispatchers.IO) { repository.load() }
        snapshot = loaded
        if (page == Page.CONFIG) configText = loaded.text
    }
    fun settings() { if (!busy) { page = Page.SETTINGS; error = null } }
    fun about() { page = Page.ABOUT }
    fun config() {
        if (busy || snapshot == null) return
        configReturn = page
        configText = snapshot!!.text; error = null; page = Page.CONFIG
    }
    fun edit(host: Host? = null) {
        if (busy || snapshot == null) return
        draft = HostDraft(host, host?.alias ?: "", host?.hostname ?: "", host?.user ?: "", host?.port?.toString() ?: "22")
        error = null; page = Page.EDIT_HOST
    }
    fun back() { if (!busy) { if (dirty) discard = true else leave() } }
    private fun leave() {
        page = when (page) { Page.ABOUT -> Page.SETTINGS; Page.CONFIG -> configReturn; else -> Page.HOSTS }
        error = null; draft = null; discard = false
    }
    fun confirmDiscard() = leave()
    fun keepEditing() { discard = false }
    fun dismissNotice() { notice = null }
    fun notConnected() { notice = "终端连接正在接入，当前版本可管理主机与 SSH 配置" }
    fun saveHost() {
        val base = snapshot ?: return
        val editing = draft ?: return
        work {
            val saved = withContext(Dispatchers.IO) { repository.saveHost(base, editing.edit()) }
            snapshot = saved.snapshot; notice = saved.warning ?: "主机已保存"; page = Page.HOSTS; draft = null
        }
    }
    fun saveConfig() {
        val base = snapshot ?: return
        val text = configText
        work {
            val saved = withContext(Dispatchers.IO) { repository.save(base, text.toByteArray(Charsets.UTF_8)) }
            snapshot = saved.snapshot; configText = saved.snapshot.text
            notice = saved.warning ?: "SSH 配置已保存"; page = configReturn
        }
    }
    fun readImport(uri: Uri) = work {
        imported = withContext(Dispatchers.IO) {
            val bytes = getApplication<Application>().contentResolver.openInputStream(uri)?.use { input ->
                val output = java.io.ByteArrayOutputStream()
                val buffer = ByteArray(8192)
                while (true) {
                    val count = input.read(buffer); if (count < 0) break
                    if (output.size() + count > 1024 * 1024) throw ConfigAccess("SSH 配置不能超过 1 MiB")
                    output.write(buffer, 0, count)
                }
                output.toByteArray()
            } ?: throw ConfigAccess("无法读取所选文档")
            ConfigRepository.validate(bytes)
            bytes
        }
    }
    fun cancelImport() { imported = null }
    fun confirmImport() {
        val bytes = imported ?: return
        val base = snapshot ?: return
        work {
            val saved = withContext(Dispatchers.IO) { repository.save(base, bytes) }
            snapshot = saved.snapshot; imported = null; notice = saved.warning ?: "配置副本已导入"; page = Page.HOSTS
        }
    }
}

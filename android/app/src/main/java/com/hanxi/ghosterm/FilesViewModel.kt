package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.FileBrowserRepository
import com.hanxi.ghosterm.bridge.RemoteFileSession
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.io.File

data class FileCrumb(val location: FileLocation, val label: String)
data class FilePreview(val entry: BrowserEntry, val text: String? = null, val error: String? = null)
data class ClipboardReplacement(val expectedId: String, val snapshot: FileClipboardSnapshot)

/** Activity-local browser over the shared file clipboard; navigation owns only its reads. */
class FilesViewModel @JvmOverloads constructor(application: Application,
    private val source: FileBrowserSource = FileBrowserRepository(LocalFileBrowser(File(application.filesDir,"home"),application.contentResolver)),
    private val work: FileWorkRuntime = (application as? GhostermApplication)?.fileWork ?: FileWorkRuntime(application)
) : AndroidViewModel(application) {
    var visible by mutableStateOf(false); private set
    var endpoints by mutableStateOf(listOf(FileEndpoint.APP)); private set
    var crumbs by mutableStateOf(listOf(FileCrumb(FileLocation(FileEndpoint.APP),FileEndpoint.APP.label))); private set
    val location get() = crumbs.last().location
    var entries by mutableStateOf<List<BrowserEntry>>(emptyList()); private set
    var loading by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var endpointError by mutableStateOf<String?>(null); private set
    var choosingLocation by mutableStateOf(false); private set
    var selecting by mutableStateOf(false); private set
    var selected by mutableStateOf<Set<String>>(emptySet()); private set
    var showHidden by mutableStateOf(false); private set
    var preview by mutableStateOf<FilePreview?>(null); private set
    var pickerPending by mutableStateOf(false); private set
    private val clipboardStore = work.clipboard
    var clipboard: FileClipboardSnapshot?
        get() = clipboardStore.snapshot
        private set(value) { clipboardStore.snapshot=value }
    var clipboardDetails by mutableStateOf(false); private set
    var clipboardReplacement by mutableStateOf<ClipboardReplacement?>(null); private set
    var clipboardClearId by mutableStateOf<String?>(null); private set
    var clipboardError by mutableStateOf<String?>(null); private set
    val visibleEntries get() = entries.filter { showHidden || !it.name.startsWith(".") }
    init {
        viewModelScope.launch { work.tasks.completed.collect { result -> result?.let { copyCompleted(it) } } }
    }
    private val remembered = mutableMapOf<String,List<FileCrumb>>()
    private var readJob: Job? = null
    private var rootsJob: Job? = null
    private var generation = 0L
    private var rootsGeneration = 0L
    fun enter() { visible=true; choosingLocation=false; refreshEndpoints(); reload() }
    fun leave() {
        visible=false;choosingLocation=false;pickerPending=false;rootsGeneration++;rootsJob?.cancel()
        dismissClipboardDialogs();clipboardError=null
        cancelRead();preview=null;clearSelection();(source as? FileBrowserRepository)?.close()
    }
    private fun cancelRead(): Long { generation++;readJob?.cancel();readJob=null;loading=false;return generation }
    fun clearSelection() { selecting=false;selected=emptySet() }
    fun selectMode() { if(!loading && error==null) selecting=true }
    fun all() { if(selecting) selected=visibleEntries.map { it.id }.toSet() }
    fun clearChecked() { selected=emptySet() }
    fun copySelected() {
        if(!visible || loading || error!=null || !selecting || selected.isEmpty()) return
        clipboardError=null
        try {
            val chosen=visibleEntries.filter { it.id in selected }
            check(chosen.size==selected.size) { "选择已失效，请重新选择" }
            val snapshot=FileClipboardSnapshot.capture(location,
                if(location.endpoint.remote!=null) location.directoryId else crumbs.joinToString(" / ") { it.label },chosen)
            val previous=clipboard
            if(previous==null) installClipboard(snapshot)
            else clipboardReplacement=ClipboardReplacement(previous.id,snapshot)
        } catch(failure: Exception) { clipboardError=failure.message ?: "无法创建复制清单" }
    }
    private fun installClipboard(snapshot: FileClipboardSnapshot) {
        clipboard=snapshot;clipboardError=null;dismissClipboardDialogs();clearSelection()
    }
    fun confirmClipboardReplacement() {
        val pending=clipboardReplacement ?: return
        clipboardReplacement=null
        if(clipboard?.id!=pending.expectedId) { clipboardError="复制清单已更新，请重新操作";return }
        installClipboard(pending.snapshot)
    }
    fun dismissClipboardReplacement() { clipboardReplacement=null }
    fun showClipboard() { if(clipboard!=null) clipboardDetails=true }
    fun hideClipboard() { clipboardDetails=false }
    fun requestClipboardClear() { clipboardClearId=clipboard?.id }
    fun dismissClipboardClear() { clipboardClearId=null }
    fun confirmClipboardClear() {
        val expected=clipboardClearId ?: return
        clipboardClearId=null
        if(clipboard?.id!=expected) { clipboardError="复制清单已更新，请重新操作";return }
        clipboard=null;clipboardDetails=false;clipboardError=null
    }
    fun copyCompleted(result: FileCopyProgress, clearUsed: Boolean = false) {
        if(clearUsed && result.mayClearClipboard && clipboard?.id==result.clipboardId) {
            clipboard=null;dismissClipboardDialogs();clipboardError=null
        }
        if(visible && preview==null && !selecting && result.items.any { it.targetParent==location }) reload()
    }
    private fun dismissClipboardDialogs() {
        clipboardDetails=false;clipboardReplacement=null;clipboardClearId=null
    }
    fun toggleHidden() { showHidden=!showHidden;selected=selected.intersect(visibleEntries.map { it.id }.toSet()) }
    fun chooseLocations() { dismissClipboardDialogs();choosingLocation=true;refreshEndpoints() }
    fun dismissLocations() { choosingLocation=false }
    private fun refreshEndpoints() {
        val token=++rootsGeneration;rootsJob?.cancel();endpointError=null
        rootsJob=viewModelScope.launch {
            try { val found=source.endpoints();if(visible && token==rootsGeneration) endpoints=found.toList() }
            catch(cancel: CancellationException) { throw cancel }
            catch(failure: Exception) { if(visible && token==rootsGeneration) endpointError=failure.message ?: "无法读取已授权文件夹" }
        }
    }
    fun choose(endpoint: FileEndpoint) {
        if(endpoints.none { it.id==endpoint.id }) return
        dismissClipboardDialogs()
        remembered[location.endpoint.id]=crumbs.toList()
        crumbs=remembered[endpoint.id] ?: listOf(FileCrumb(FileLocation(endpoint),endpoint.label))
        choosingLocation=false;reload()
    }
    fun beginRemoteConnection(): Long { endpointError=null;return cancelRead() }
    fun acceptsRemoteConnection(token: Long) = visible && token==generation
    fun remoteConnectionError(message: String) { endpointError=message;choosingLocation=true }
    fun remoteConnected(identity: RemoteFileIdentity, connection: RemoteFileSession, token: Long) {
        if(!acceptsRemoteConnection(token)) { connection.close();return }
        val repository=source as? FileBrowserRepository
        if(repository==null) { connection.close();error="远端文件服务不可用";return }
        cancelRead()
        val alias=identity.alias
        // Reconnection replaces this browser endpoint only; terminal sessions are unrelated.
        endpoints.filter { it.remoteAlias==alias }.forEach(repository::disconnect)
        val endpoint=repository.attach(identity,connection)
        endpoints=endpoints.filterNot { it.remoteAlias==alias }+endpoint
        choose(endpoint)
    }
    fun disconnectRemote() {
        val endpoint=location.endpoint
        if(endpoint.remoteAlias==null) return
        cancelRead();(source as? FileBrowserRepository)?.disconnect(endpoint)
        endpoints=endpoints.filterNot { it.id==endpoint.id };remembered.remove(endpoint.id)
        choose(FileEndpoint.APP)
    }
    override fun onCleared() { leave();(source as? FileBrowserRepository)?.close();super.onCleared() }
    fun beginPicker(): Boolean { if(pickerPending) return false;pickerPending=true;return true }
    fun pickerFailed() { pickerPending=false;endpointError="无法打开系统文件夹选择器";choosingLocation=true }
    fun picked(uri: String?, flags: Int) {
        if(!pickerPending) return
        pickerPending=false
        if(uri==null || !visible) return
        val token=++rootsGeneration;rootsJob?.cancel();endpointError=null
        rootsJob=viewModelScope.launch {
            try {
                val endpoint=source.grantTree(uri,flags)
                if(visible && token==rootsGeneration) {
                    endpoints=endpoints.filterNot { it.id==endpoint.id }+endpoint;choose(endpoint)
                }
            } catch(cancel: CancellationException) { throw cancel }
            catch(failure: Exception) { if(visible && token==rootsGeneration) { endpointError=failure.message ?: "无法保留文件夹授权";choosingLocation=true } }
        }
    }
    fun reload() {
        val token=cancelRead();val requested=location
        clipboardReplacement=null
        clearSelection();preview=null;entries=emptyList();error=null;loading=true
        readJob=viewModelScope.launch {
            try {
                val found=source.browse(requested)
                if(visible && token==generation) {
                    crumbs=crumbs.dropLast(1)+FileCrumb(found.location,
                        if(found.location.endpoint.remoteAlias!=null) found.location.directoryId else crumbs.last().label)
                    entries=found.entries.toList()
                }
            }
            catch(cancel: CancellationException) { throw cancel }
            catch(failure: Exception) { if(visible && token==generation) error=failure.message ?: "读取文件夹失败，请重试" }
            finally { if(token==generation) loading=false }
        }
    }
    fun longPress(entry: BrowserEntry) {
        if(loading || error!=null || entry !in visibleEntries) return
        selecting=true;selected=selected+entry.id
    }
    fun tap(entry: BrowserEntry) {
        if(loading || error!=null || entry !in visibleEntries) return
        if(selecting) { selected=if(entry.id in selected) selected-entry.id else selected+entry.id;return }
        if(entry.kind==FileKind.DIRECTORY) {
            crumbs=crumbs+FileCrumb(FileLocation(location.endpoint,entry.id),entry.name);reload();return
        }
        val token=cancelRead();val requested=location;preview=FilePreview(entry)
        readJob=viewModelScope.launch {
            try { val text=source.preview(requested,entry);if(visible && token==generation) preview=FilePreview(entry,text) }
            catch(cancel: CancellationException) { throw cancel }
            catch(failure: Exception) { if(visible && token==generation) preview=FilePreview(entry,error=failure.message ?: "无法预览文件") }
        }
    }
    fun closePreview() { cancelRead();preview=null }
    fun back() {
        when {
            clipboardClearId!=null -> dismissClipboardClear()
            clipboardReplacement!=null -> dismissClipboardReplacement()
            clipboardDetails -> hideClipboard()
            choosingLocation -> dismissLocations()
            preview!=null -> closePreview()
            selecting -> clearSelection()
            crumbs.size>1 -> { crumbs=crumbs.dropLast(1);reload() }
            else -> leave()
        }
    }
}

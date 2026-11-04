package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.data.PreviewFileSource
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.data.FileBrowserRepository
import com.hanxi.ghosterm.data.PreferencesState
import com.hanxi.ghosterm.data.PreferenceUpdate
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
    private val work: FileWorkRuntime = (application as? GhostermApplication)?.fileWork ?: FileWorkRuntime(application),
    private val externalCache: ExternalPreviewCache = (application as? GhostermApplication)?.externalPreviews
        ?: ExternalPreviewCache(application,File(application.cacheDir,"external-previews"), "${application.packageName}.previews")
) : AndroidViewModel(application) {
    var visible by mutableStateOf(false); private set
    var endpoints by mutableStateOf(listOf(FileEndpoint.APP)); private set
    var crumbs by mutableStateOf(listOf(FileCrumb(FileLocation(FileEndpoint.APP),FileEndpoint.APP.label))); private set
    val location get() = crumbs.last().location
    var remoteScope by mutableStateOf(false); private set
    val awaitingRemote get() = remoteScope && location.endpoint.remoteAlias==null
    private var lastLocalEndpointId = FileEndpoint.APP.id
    private var lastRemoteEndpointId: String? = null
    var entries by mutableStateOf<List<BrowserEntry>>(emptyList()); private set
    var loading by mutableStateOf(false); private set
    var error by mutableStateOf<String?>(null); private set
    var endpointError by mutableStateOf<String?>(null); private set
    var choosingLocation by mutableStateOf(false); private set
    var selecting by mutableStateOf(false); private set
    var selected by mutableStateOf<Set<String>>(emptySet()); private set
    private var listing by mutableStateOf(work.preferences)
    val showHidden get() = listing.showHidden
    var hiddenSaving by mutableStateOf(false); private set
    var preferencesAvailable by mutableStateOf(work.settings == null); private set
    var preferenceError by mutableStateOf<String?>(null); private set
    var preview by mutableStateOf<FilePreview?>(null); private set
    var pickerPending by mutableStateOf(false); private set
    var deviceAccessPending by mutableStateOf(false); private set
    private val clipboardStore = work.clipboard
    var clipboard: FileClipboardSnapshot?
        get() = clipboardStore.snapshot
        private set(value) { clipboardStore.snapshot=value }
    var clipboardDetails by mutableStateOf(false); private set
    var clipboardReplacement by mutableStateOf<ClipboardReplacement?>(null); private set
    var clipboardClearId by mutableStateOf<String?>(null); private set
    var clipboardError by mutableStateOf<String?>(null); private set
    val visibleEntries get() = listing.orderFiles(entries)
    init {
        viewModelScope.launch { work.tasks.completed.collect { result -> result?.let { copyCompleted(it) } } }
        work.settings?.let { settings -> viewModelScope.launch {
            settings.state.collect { state ->
                listing = state.committed
                preferencesAvailable = state.storage is PreferencesState.Ready
                preferenceError = (state.storage as? PreferencesState.Unavailable)?.let {
                    "文件偏好读取或保存结果无法确认，请在设置中重新载入。"
                }
                selected = selected.intersect(visibleEntries.map { it.id }.toSet())
            }
        } }
    }
    private val remembered = mutableMapOf<String,List<FileCrumb>>()
    var externalPreparing by mutableStateOf(false); private set
    var externalError by mutableStateOf<String?>(null); private set
    var externalCacheError by mutableStateOf<String?>(null); private set
    var externalReady by mutableStateOf<ExternalPreviewCache.Ready?>(null); private set
    var externalClearConfirm by mutableStateOf(false); private set
    var externalClearing by mutableStateOf(false); private set
    var externalClearResult by mutableStateOf<ExternalPreviewCache.ClearResult?>(null); private set
    var externalClearError by mutableStateOf<String?>(null); private set
    fun requestExternalClear() {
        if(!visible || externalClearing) return
        externalClearResult=null;externalClearError=null;externalClearConfirm=true
    }
    fun dismissExternalClear() { externalClearConfirm=false }
    fun confirmExternalClear() {
        if(!visible || !externalClearConfirm || externalClearing) return
        externalClearConfirm=false;externalClearing=true
        viewModelScope.launch {
            try { externalClearResult=externalCache.clearCopies() }
            catch(cancel: CancellationException) { throw cancel }
            catch(error: Exception) { externalClearError=error.message ?: "无法清理预览副本" }
            finally { externalClearing=false }
        }
    }
    private var externalJob: Job? = null
    private var externalGeneration=0L
    init { viewModelScope.launch { externalCache.cleanupError.collect { externalCacheError=it } } }
    private fun cancelExternal() {
        externalGeneration++;externalJob?.cancel();externalJob=null;externalPreparing=false;externalError=null
        externalReady?.let(externalCache::discardLater);externalReady=null
    }
    fun externalPresenterStopped() {
        val pending=externalPreparing || externalReady!=null
        cancelExternal()
        if(pending) externalError="外部打开已取消，请再次点击打开"
    }
    fun openExternal() {
        val shown=preview ?: return
        if(!visible || externalPreparing || externalReady!=null || shown.entry.kind!=FileKind.FILE ||
            (shown.text==null && shown.error==null)) return
        cancelExternal();externalError=null;externalPreparing=true
        val token=externalGeneration;val requested=location
        externalJob=viewModelScope.launch {
            var ready: ExternalPreviewCache.Ready?=null
            try {
                val reader=checkNotNull(source as? PreviewFileSource) { "此文件服务不支持外部打开" }
                ready=externalCache.prepare(shown.entry.name,shown.entry.size) { reader.snapshot(requested,shown.entry,it) }
                if(visible && token==externalGeneration && preview?.entry==shown.entry) {
                    externalReady=ready;ready=null
                }
            } catch(cancel: CancellationException) { throw cancel }
            catch(error: Exception) { if(token==externalGeneration) externalError=error.message ?: "无法准备外部预览" }
            finally {
                ready?.let(externalCache::discardLater)
                if(token==externalGeneration) { externalPreparing=false;externalJob=null }
            }
        }
    }
    /** Called synchronously by the visible page; a stale preparation never launches an activity. */
    fun launchExternal(ready: ExternalPreviewCache.Ready, launch: (ExternalPreviewCache.Ready) -> Unit) {
        if(!visible || externalReady!==ready || preview==null) return
        externalReady=null
        try { launch(ready);externalCache.handedOff(ready) }
        catch(error: Exception) {
            externalCache.discardLater(ready)
            externalError=if(error is android.content.ActivityNotFoundException) "没有可打开此类型文件的应用，请先复制到设备文件夹"
                else "无法打开其他应用：${error.message ?: "未知原因"}"
        }
    }
    private var readJob: Job? = null
    private var rootsJob: Job? = null
    private var generation = 0L
    private var rootsGeneration = 0L
    fun enter() { enterForConnection();reload() }
    /** A terminal entry chooses its retained connection before issuing one read. */
    fun enterForConnection() {
        externalCache.sweepLater();remoteScope=location.endpoint.remoteAlias!=null
        visible=true;choosingLocation=false;refreshEndpoints()
        cancelRead();preview=null;entries=emptyList();error=null;clearSelection()
    }
    fun enterLocal() {
        lastLocalEndpointId=FileEndpoint.APP.id
        remembered[location.endpoint.id]=crumbs.toList()
        crumbs=remembered[FileEndpoint.APP.id] ?: listOf(FileCrumb(FileLocation(FileEndpoint.APP),FileEndpoint.APP.label))
        enter()
    }
    fun leave() {
        externalClearConfirm=false
        visible=false;choosingLocation=false;pickerPending=false;deviceAccessPending=false;rootsGeneration++;rootsJob?.cancel()
        dismissClipboardDialogs();clipboardError=null
        cancelRead();preview=null;clearSelection()
    }
    private fun cancelRead(): Long { cancelExternal();generation++;readJob?.cancel();readJob=null;loading=false;return generation }
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
    fun toggleHidden() {
        if (hiddenSaving || !preferencesAvailable) return
        val settings = work.settings
        if (settings == null) {
            listing = listing.copy(showHidden = !showHidden)
            selected = selected.intersect(visibleEntries.map { it.id }.toSet())
            return
        }
        val desired = !showHidden
        hiddenSaving = true; preferenceError = null
        viewModelScope.launch {
            try {
                val result = settings.save(java.util.UUID.randomUUID(), listOf(PreferenceEdit.ShowHidden(desired)))
                if (result is PreferenceUpdate.Failed) preferenceError = result.message
            } finally { hiddenSaving = false }
        }
    }
    fun switchScope(remote: Boolean) {
        if(selecting || remote==remoteScope) return
        deviceAccessPending=false
        pickerPending=false
        rootsGeneration++;rootsJob?.cancel();rootsJob=null
        val endpoint=if(remote) endpoints.firstOrNull { it.id==lastRemoteEndpointId && it.remoteAlias!=null }
            ?: endpoints.lastOrNull { it.remoteAlias!=null }
        else endpoints.firstOrNull { it.id==lastLocalEndpointId && it.remoteAlias==null } ?: FileEndpoint.APP
        if(endpoint!=null) { choose(endpoint);return }
        // Empty remote scope has no destination. Invalidate local reads and any
        // pending connection before showing the explicit connection entry.
        cancelRead();dismissClipboardDialogs();clearSelection();preview=null;entries=emptyList();error=null
        remoteScope=true
    }
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
        deviceAccessPending=false
        dismissClipboardDialogs()
        remembered[location.endpoint.id]=crumbs.toList()
        crumbs=remembered[endpoint.id] ?: listOf(FileCrumb(FileLocation(endpoint),endpoint.label))
        remoteScope=endpoint.remoteAlias!=null
        if(remoteScope) lastRemoteEndpointId=endpoint.id else lastLocalEndpointId=endpoint.id
        choosingLocation=false;reload()
    }
    private val remoteTargets = mutableMapOf<String, com.hanxi.ghosterm.bridge.PreparedSSH>()
    /** Only reuse this browser's live owner with the same immutable connection inputs. */
    fun reuseRemote(target: com.hanxi.ghosterm.bridge.PreparedSSH): Boolean {
        val repository = source as? FileBrowserRepository ?: return false
        val endpoint = endpoints.firstOrNull { endpoint ->
            remoteTargets[endpoint.id]?.let { old ->
                old.alias == target.alias && old.host == target.host && old.port == target.port &&
                    (target.user.isBlank() || old.user == target.user) && old.identityFiles == target.identityFiles
            } == true && repository.isConnected(endpoint)
        } ?: return false
        choose(endpoint)
        return true
    }
    fun disconnectAll() {
        cancelRead(); (source as? FileBrowserRepository)?.close(); remoteTargets.clear()
    }
    fun beginRemoteConnection(): Long { endpointError=null;return cancelRead() }
    fun acceptsRemoteConnection(token: Long) = visible && token==generation
    fun remoteConnectionError(message: String) { endpointError=message;choosingLocation=true }
    fun remoteConnected(identity: RemoteFileIdentity, connection: RemoteFileSession, token: Long, target: com.hanxi.ghosterm.bridge.PreparedSSH? = null) {
        if(!acceptsRemoteConnection(token)) { connection.close();return }
        val repository=source as? FileBrowserRepository
        if(repository==null) { connection.close();error="远端文件服务不可用";return }
        cancelRead()
        val alias=identity.alias
        // Reconnection replaces this browser endpoint only; terminal sessions are unrelated.
        endpoints.filter { it.remoteAlias==alias }.forEach { repository.disconnect(it);remoteTargets.remove(it.id) }
        val endpoint=repository.attach(identity,connection)
        if(target!=null) remoteTargets[endpoint.id]=target.copy(user=identity.user,identityFiles=target.identityFiles.toList())
        endpoints=endpoints.filterNot { it.remoteAlias==alias }+endpoint
        choose(endpoint)
    }
    fun disconnectRemote() {
        val endpoint=location.endpoint
        if(endpoint.remoteAlias==null) return
        cancelRead();(source as? FileBrowserRepository)?.disconnect(endpoint);remoteTargets.remove(endpoint.id)
        endpoints=endpoints.filterNot { it.id==endpoint.id };remembered.remove(endpoint.id)
        choose(FileEndpoint.APP)
    }
    override fun onCleared() { leave();disconnectAll();super.onCleared() }
    fun beginPicker(): Boolean { if(pickerPending) return false;pickerPending=true;return true }
    fun beginDeviceAccess(): Boolean {
        if(!visible || selecting || deviceAccessPending) return false
        deviceAccessPending=true;return true
    }
    fun completeDeviceAccess() {
        if(!deviceAccessPending || !visible) return
        deviceAccessPending=false
        if(!com.hanxi.ghosterm.data.DeviceStorageAccess.granted(getApplication())) {
            endpointError="设备存储访问未授权，仍可使用「选择设备文件夹」";choosingLocation=true;return
        }
        endpoints=endpoints.filterNot { it.id==FileEndpoint.DEVICE.id }+FileEndpoint.DEVICE
        choose(FileEndpoint.DEVICE)
    }
    fun deviceAccessFailed() {
        deviceAccessPending=false;endpointError="无法打开存储权限设置，请使用「选择设备文件夹」";choosingLocation=true
    }
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
        if(awaitingRemote) return
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
            awaitingRemote -> switchScope(false)
            crumbs.size>1 -> { crumbs=crumbs.dropLast(1);reload() }
            else -> leave()
        }
    }
}

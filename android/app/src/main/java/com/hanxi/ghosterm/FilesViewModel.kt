package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.data.LocalFileBrowser
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import java.io.File

data class FileCrumb(val location: FileLocation, val label: String)
data class FilePreview(val entry: BrowserEntry, val text: String? = null, val error: String? = null)

/** One process-local browser; navigation owns reads, never terminal connections. */
class FilesViewModel @JvmOverloads constructor(application: Application,
    private val source: FileBrowserSource = LocalFileBrowser(File(application.filesDir,"home"),application.contentResolver)
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
    val visibleEntries get() = entries.filter { showHidden || !it.name.startsWith(".") }
    private val remembered = mutableMapOf<String,List<FileCrumb>>()
    private var readJob: Job? = null
    private var rootsJob: Job? = null
    private var generation = 0L
    private var rootsGeneration = 0L
    fun enter() { visible=true; choosingLocation=false; refreshEndpoints(); reload() }
    fun leave() {
        visible=false;choosingLocation=false;pickerPending=false;rootsGeneration++;rootsJob?.cancel()
        cancelRead();preview=null;clearSelection()
    }
    private fun cancelRead(): Long { generation++;readJob?.cancel();readJob=null;loading=false;return generation }
    fun clearSelection() { selecting=false;selected=emptySet() }
    fun selectMode() { if(!loading && error==null) selecting=true }
    fun all() { if(selecting) selected=visibleEntries.map { it.id }.toSet() }
    fun clearChecked() { selected=emptySet() }
    fun toggleHidden() { showHidden=!showHidden;selected=selected.intersect(visibleEntries.map { it.id }.toSet()) }
    fun chooseLocations() { choosingLocation=true;refreshEndpoints() }
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
        remembered[location.endpoint.id]=crumbs.toList()
        crumbs=remembered[endpoint.id] ?: listOf(FileCrumb(FileLocation(endpoint),endpoint.label))
        choosingLocation=false;reload()
    }
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
        clearSelection();preview=null;entries=emptyList();error=null;loading=true
        readJob=viewModelScope.launch {
            try { val found=source.list(requested);if(visible && token==generation) entries=found.toList() }
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
            choosingLocation -> dismissLocations()
            preview!=null -> closePreview()
            selecting -> clearSelection()
            crumbs.size>1 -> { crumbs=crumbs.dropLast(1);reload() }
            else -> leave()
        }
    }
}

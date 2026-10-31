package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File

/** Process-local task owner. A single active request reserves the slot through cleanup.
 * Activity recreation and leaving the file page do not cancel confirmed work. */
enum class FileTaskPhase { SCANNING, CONFIRMING, RUNNING, CANCELLING, FINISHED }
class FileTask(val request: FilePasteRequest) {
    var phase by mutableStateOf(FileTaskPhase.SCANNING); internal set
    var decisions by mutableStateOf<FilePasteDecisionState?>(null); internal set
    var createCapability by mutableStateOf(FileCreateCapability.UNKNOWN); internal set
    var result by mutableStateOf<FileCopyProgress?>(null); internal set
    var error by mutableStateOf<String?>(null); internal set
    var cancelled by mutableStateOf(false); internal set
    internal var decisionOwner: FilePasteDecisions? = null
    internal var execution: FileCopyExecutor.Request? = null
    internal var job: Job? = null
    internal lateinit var connections: (FilePasteRequest) -> FileTaskConnections
    internal var complete: ((FileCopyProgress) -> Unit)? = null
    val active get() = phase != FileTaskPhase.FINISHED
}

class FileTasksViewModel(application: Application) : AndroidViewModel(application) {
    val tasks = mutableStateListOf<FileTask>()
    var visible by mutableStateOf(false); private set
    var notice by mutableStateOf<String?>(null); private set
    val busy get() = tasks.any { it.active }
    private val home = File(application.filesDir,"home")
    private val staging = File(application.filesDir,"file-tasks")
    private val resolver = application.contentResolver
    fun show() { visible=true }
    fun hide() { visible=false }
    fun start(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String,
        connections: (FilePasteRequest) -> FileTaskConnections, complete: (FileCopyProgress) -> Unit) {
        if(busy) { notice="已有复制请求，请先完成或取消当前任务";visible=true;return }
        val request=FilePasteRequest.capture(snapshot,destination,label)
        val task=FileTask(request).also { it.connections=connections;it.complete=complete }
        notice=null;tasks.add(0,task);visible=true
        task.job=viewModelScope.launch(start=CoroutineStart.LAZY) {
            try {
                val check=withContext(Dispatchers.IO) {
                    connections(request).use { scope ->
                        val local=LocalFileBrowser(home,resolver)
                        suspend fun browse(role: FileTaskConnections.Role, location: FileLocation): BrowserDirectory {
                            val remote=scope.remote(role,location.endpoint)?.session ?: return local.browse(location)
                            val found=remote.browse(location.directoryId)
                            return BrowserDirectory(location.copy(directoryId=found.path),found.entries.map { entry ->
                                BrowserEntry(entry.path,entry.name,when {
                                    entry.symbolicLink->FileKind.LINK;entry.directory->FileKind.DIRECTORY
                                    entry.regularFile->FileKind.FILE;else->FileKind.OTHER
                                },entry.size,entry.modifiedSeconds?.takeIf { it<=Long.MAX_VALUE/1000 }?.times(1000))
                            })
                        }
                        val scan=FileTreeScanner { browse(FileTaskConnections.Role.SOURCE,it) }.scan(request)
                        check(scan.complete) { scan.failure?.message ?: "来源扫描不完整" }
                        val access=FileDestinationAccess(home,resolver) { browse(FileTaskConnections.Role.DESTINATION,it) }
                        FilePastePreflight(access::inspect,access::relation).check(scan)
                    }
                }
                ensureActive()
                check(check.checked) { check.failure?.message ?: "无法确认复制目标" }
                task.createCapability=check.create
                task.decisionOwner=FilePasteDecisions.begin(check)
                task.decisions=task.decisionOwner!!.state();task.phase=FileTaskPhase.CONFIRMING
            } catch(timeout: TimeoutCancellationException) {
                task.error="任务连接超时，请重新确认后重试"
            } catch(cancel: CancellationException) {
                task.cancelled=true;task.error="复制准备已取消"
            } catch(failure: Exception) { task.error=failure.message ?: "无法准备复制" }
            finally {
                task.job=null
                if(task.phase!=FileTaskPhase.CONFIRMING) task.phase=FileTaskPhase.FINISHED
            }
        }.also { it.start() }
    }
    fun choose(task: FileTask, ref: FilePasteDecisionRef, path: String, choice: FilePasteChoice, all: Boolean) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING) return
        try { task.decisions=checkNotNull(task.decisionOwner).choose(ref,path,choice,all);task.error=null }
        catch(failure: Exception) { task.error=failure.message ?: "确认已失效" }
    }
    fun confirm(task: FileTask, ref: FilePasteDecisionRef) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING) return
        try {
            val plan=checkNotNull(task.decisionOwner).confirm(ref)
            val execution=FileCopyExecutor(home,resolver,staging,task.connections).prepare(plan)
            task.execution=execution;task.decisions=null;task.error=null;task.phase=FileTaskPhase.RUNNING
            task.job=viewModelScope.launch(start=CoroutineStart.LAZY) {
                val observer=launch { execution.updates.collect { task.result=it } }
                try { execution.execute() }
                catch(cancel: CancellationException) { execution.cancel() }
                catch(failure: Exception) { task.error=failure.message ?: "复制未完成" }
                finally {
                    withContext(NonCancellable) { execution.awaitClosed() }
                    observer.cancel();task.result=execution.outcome
                    task.phase=FileTaskPhase.FINISHED;task.job=null
                    task.complete?.invoke(execution.outcome);task.complete=null
                }
            }.also { it.start() }
        } catch(failure: Exception) { task.error=failure.message ?: "无法开始复制" }
    }
    fun cancelRequest(id: String) { tasks.firstOrNull { it.request.id==id }?.let(::cancel) }
    fun cancel(task: FileTask) {
        if(task !in tasks || !task.active || task.phase==FileTaskPhase.CANCELLING) return
        task.cancelled=true
        if(task.phase==FileTaskPhase.CONFIRMING) {
            task.decisions?.let { task.decisionOwner?.cancel(it.reference) }
            task.decisions=null;task.phase=FileTaskPhase.FINISHED;task.error="复制已取消，尚未写入目标";task.complete=null
        } else {
            task.phase=FileTaskPhase.CANCELLING
            task.execution?.cancel() ?: task.job?.cancel()
        }
    }
    fun remove(task: FileTask) { if(!task.active) tasks.remove(task) }
    override fun onCleared() { tasks.toList().forEach(::cancel);super.onCleared() }
}

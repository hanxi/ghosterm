package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import androidx.lifecycle.AndroidViewModel
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import java.io.File

/** Application-owned execution; pages never own the task job or its authentication. */
enum class FileTaskPhase { QUEUED, SCANNING, CONFIRMING, RUNNING, CANCELLING, FINISHED }
class FileTask(val request: FilePasteRequest) {
    var phase by mutableStateOf(FileTaskPhase.QUEUED); internal set
    var decisions by mutableStateOf<FilePasteDecisionState?>(null); internal set
    var createCapability by mutableStateOf(FileCreateCapability.UNKNOWN); internal set
    var result by mutableStateOf<FileCopyProgress?>(null); internal set
    var error by mutableStateOf<String?>(null); internal set
    var cancelled by mutableStateOf(false); internal set
    internal var decisionOwner: FilePasteDecisions? = null
    internal var execution: FileCopyExecutor.Request? = null
    internal var job: Job? = null
    internal var recorded = false
    internal var recording = false
    var persistenceError by mutableStateOf<String?>(null); internal set
    internal lateinit var connections: (FilePasteRequest) -> FileTaskConnections
    val active get() = phase != FileTaskPhase.FINISHED
}

class FileTaskStore(application: Application, private val scope: CoroutineScope) {
    companion object { internal const val MAX_REQUESTS = 32 }
    val tasks = mutableStateListOf<FileTask>()
    var notice by mutableStateOf<String?>(null); private set
    val busy get() = tasks.any { it.active || it.recording }
    val history = mutableStateListOf<FileJournalRecord>()
    var backgroundHold by mutableStateOf<String?>(null); private set
    internal fun holdForBackground(reason: String) { backgroundHold=reason }
    internal fun allowBackgroundScheduling() { backgroundHold=null;advance() }
    internal fun stopForBackground(reason: String) {
        holdForBackground(reason)
        tasks.filter { it.active }.toList().forEach(::cancel)
    }
    var journalLoading by mutableStateOf(true); private set
    var journalError by mutableStateOf<String?>(null); private set
    private val journal = FileTaskJournal(File(application.filesDir,"file-history/tasks.db"))
    private val initialized = scope.async {
        try { history.addAll(journal.load()) }
        catch(error: Exception) { journalError=error.message ?: "无法读取任务记录" }
        finally { journalLoading=false }
    }
    fun reloadHistory() {
        if(busy || journalLoading) return
        journalLoading=true
        scope.launch {
            try {
                val found=journal.load()
                history.clear();history.addAll(found.filter { record -> tasks.none { it.request.id==record.intent.id } })
                journalError=null
            } catch(error: Exception) { journalError=error.message ?: "无法读取任务记录" }
            finally { journalLoading=false }
        }
    }
    private fun recordFailure(task: FileTask, error: Exception) {
        task.persistenceError="任务记录保存失败：${error.message ?: "未知原因"}。已停止推进，请核对实际位置。"
        journalError=task.persistenceError
    }
    private suspend fun finish(task: FileTask) {
        withContext(NonCancellable) {
            if(task.recorded) try { journal.finish(task.request.id,task.result,task.cancelled,task.error ?: task.persistenceError) }
            catch(error: Exception) { recordFailure(task,error) }
        }
        task.recording=false;task.phase=FileTaskPhase.FINISHED;task.job=null
    }
    private val home = File(application.filesDir,"home")
    private val staging = File(application.filesDir,"file-tasks")
    private val resolver = application.contentResolver
    private val completedState = kotlinx.coroutines.flow.MutableStateFlow<FileCopyProgress?>(null)
    val completed: kotlinx.coroutines.flow.StateFlow<FileCopyProgress?> = completedState
    fun start(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String,
        connections: (FilePasteRequest) -> FileTaskConnections) {
        if(journalError!=null) { notice="任务记录不可用，请先在文件任务中核对记录错误";return }
        if(tasks.count { it.active } >= MAX_REQUESTS) {
            notice="待处理请求已达 $MAX_REQUESTS 个，请先完成或取消已有任务";return
        }
        val request=FilePasteRequest.capture(snapshot,destination,label)
        val task=FileTask(request).also { it.connections=connections }
        notice=null;tasks.add(0,task);task.recording=true
        task.job=scope.launch(start=CoroutineStart.LAZY) {
            try {
                initialized.await()
                check(journalError==null) { journalError ?: "任务记录不可用" }
                journal.accept(request);task.recorded=true
            } catch(error: Exception) { recordFailure(task,error);task.error="任务未开始，未读取或写入文件" }
            finally {
                task.recording=false;task.job=null
                if(task.cancelled || !task.recorded) finish(task)
                advance()
            }
        }.also { it.start() }
    }
    fun queuedAhead(task: FileTask): Int? {
        if(task !in tasks || task.phase!=FileTaskPhase.QUEUED) return null
        return tasks.asReversed().filter { it.active }.indexOf(task).takeIf { it>=0 }
    }
    private fun advance() {
        // Scanning, confirmation, execution and actual cancellation cleanup share one slot.
        if(journalLoading || journalError!=null || backgroundHold!=null) return
        if(tasks.any { it.active && it.phase!=FileTaskPhase.QUEUED }) return
        val task=tasks.lastOrNull { it.phase==FileTaskPhase.QUEUED } ?: return
        if(!task.recorded || task.recording) return
        task.phase=FileTaskPhase.SCANNING
        scan(task)
    }
    private fun scan(task: FileTask) {
        val request=task.request
        val connections=task.connections
        task.job=scope.launch(start=CoroutineStart.LAZY) {
            try {
                try { journal.phase(request.id,FileJournalPhase.SCANNING) }
                catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
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
                try { journal.phase(request.id,FileJournalPhase.CONFIRMING) }
                catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                ensureActive()
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
                if(task.phase!=FileTaskPhase.CONFIRMING) finish(task)
                advance()
            }
        }.also { it.start() }
    }
    fun choose(task: FileTask, ref: FilePasteDecisionRef, path: String, choice: FilePasteChoice, all: Boolean) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING) return
        try { task.decisions=checkNotNull(task.decisionOwner).choose(ref,path,choice,all);task.error=null }
        catch(failure: Exception) { task.error=failure.message ?: "确认已失效" }
    }
    fun confirm(task: FileTask, ref: FilePasteDecisionRef) {
        if(task !in tasks || task.phase!=FileTaskPhase.CONFIRMING || journalError!=null) return
        try {
            val plan=checkNotNull(task.decisionOwner).confirm(ref)
            val execution=FileCopyExecutor(home,resolver,staging,task.connections,record={ value,point ->
                journal.checkpoint(task.request.id,value,point)
            }).prepare(plan)
            task.execution=execution;task.decisions=null;task.error=null;task.phase=FileTaskPhase.RUNNING
            task.job=scope.launch(start=CoroutineStart.LAZY) {
                val observer=launch { execution.updates.collect { task.result=it } }
                var began=false
                try {
                    try { journal.begin(task.request.id,execution.outcome.items);began=true }
                    catch(error: Exception) { if(error !is CancellationException) recordFailure(task,error);throw error }
                    ensureActive();execution.execute()
                }
                catch(cancel: CancellationException) { execution.cancel() }
                catch(failure: Exception) { execution.cancel();task.error=failure.message ?: "复制未完成" }
                finally {
                    withContext(NonCancellable) { execution.awaitClosed() }
                    observer.cancel();task.result=execution.outcome.takeIf { began }
                    execution.recordingError?.let { task.persistenceError=it;journalError=it }
                    finish(task)
                    completedState.value=task.result
                    advance()
                }
            }.also { it.start() }
        } catch(failure: Exception) { task.error=failure.message ?: "无法开始复制" }
    }
    fun cancelRequest(id: String) { tasks.firstOrNull { it.request.id==id }?.let(::cancel) }
    fun cancel(task: FileTask) {
        if(task !in tasks || !task.active || task.phase==FileTaskPhase.CANCELLING) return
        task.cancelled=true
        if(task.phase==FileTaskPhase.QUEUED || task.phase==FileTaskPhase.CONFIRMING) {
            val queued=task.phase==FileTaskPhase.QUEUED
            task.decisions?.let { task.decisionOwner?.cancel(it.reference) }
            task.decisions=null;task.phase=FileTaskPhase.CANCELLING
            task.error=if(queued) "排队任务已取消，尚未读取或写入" else "复制已取消，尚未写入目标"
            // An in-flight admission must finish (or fail) before its cancellation is recorded.
            if(!task.recording) task.job=scope.launch(start=CoroutineStart.LAZY) {
                finish(task);advance()
            }.also { it.start() }
        } else {
            task.phase=FileTaskPhase.CANCELLING
            task.execution?.cancel() ?: task.job?.cancel()
        }
    }
    fun remove(task: FileTask) {
        if(task.active || task.recording) return
        task.recording=true
        scope.launch {
            try { if(task.recorded) journal.remove(task.request.id);tasks.remove(task) }
            catch(error: Exception) { recordFailure(task,error) }
            finally { task.recording=false }
        }
    }
    fun removeHistory(record: FileJournalRecord) {
        if(record !in history || journalLoading || busy) return
        journalLoading=true
        scope.launch {
            try { journal.remove(record.intent.id);history.remove(record) }
            catch(error: Exception) { journalError=error.message ?: "无法清除任务记录" }
            finally { journalLoading=false }
        }
    }
}

/** Activity-local navigation over the one application task owner. */
class FileTasksViewModel @JvmOverloads constructor(application: Application,
    internal val work: FileWorkRuntime = (application as? GhostermApplication)?.fileWork ?: FileWorkRuntime(application)
) : AndroidViewModel(application) {
    val presenterId = java.util.UUID.randomUUID()
    val tasks get() = work.tasks.tasks
    val busy get() = work.tasks.busy
    val notice get() = work.tasks.notice
    val history get() = work.tasks.history
    val journalLoading get() = work.tasks.journalLoading
    val journalError get() = work.tasks.journalError
    val background get() = work.background
    fun retryBackground() = work.background.requestFromUser(presenterId)
    fun reloadHistory() = work.tasks.reloadHistory()
    fun removeHistory(record: FileJournalRecord) = work.tasks.removeHistory(record)
    val authentication get() = work.authentication
    val presentsAuthentication get() = work.presenter == presenterId
    var visible by mutableStateOf(false); private set
    fun show() { visible=true }
    fun hide() { visible=false }
    fun start(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String,
        connections: (FilePasteRequest) -> FileTaskConnections = work::connections) {
        visible=true
        work.tasks.start(snapshot,destination,label,connections)
    }
    fun startUserTask(snapshot: FileClipboardSnapshot, destination: FileLocation, label: String) {
        visible=true
        work.background.submitFromUser(presenterId) { work.tasks.start(snapshot,destination,label,work::connections) }
    }
    fun choose(task: FileTask, ref: FilePasteDecisionRef, path: String, choice: FilePasteChoice, all: Boolean) =
        work.tasks.choose(task,ref,path,choice,all)
    fun confirm(task: FileTask, ref: FilePasteDecisionRef) = work.tasks.confirm(task,ref)
    fun cancelRequest(id: String) = work.tasks.cancelRequest(id)
    fun cancel(task: FileTask) = work.tasks.cancel(task)
    fun remove(task: FileTask) = work.tasks.remove(task)
    fun queuedAhead(task: FileTask) = work.tasks.queuedAhead(task)
    fun resumed() = work.resume(presenterId)
    fun paused() = work.pause(presenterId)
    fun focused() = work.focus(presenterId)
    override fun onCleared() { work.release(presenterId);super.onCleared() }
}

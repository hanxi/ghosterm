package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import com.hanxi.ghosterm.data.FileTaskConnections
import com.hanxi.ghosterm.domain.FileClipboardSnapshot
import com.hanxi.ghosterm.domain.FilePasteRequest
import com.hanxi.ghosterm.domain.AppPreferences
import kotlinx.coroutines.*
import java.util.UUID

/** One in-memory file workspace per application process, without Activity references. */
class FileClipboardStore {
    var snapshot by mutableStateOf<FileClipboardSnapshot?>(null)
        internal set
}

class FileWorkRuntime(application: Application,
    val settings: AppSettings? = (application as? GhostermApplication)?.settings) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val clipboard = FileClipboardStore()
    val preferences get() = settings?.state?.value?.committed ?: AppPreferences()
    val tasks = FileTaskStore(application,scope) { task, result ->
        if (task.clearClipboardAfterSuccess && !task.cancelled && task.error == null && task.persistenceError == null &&
            result.mayClearClipboard && clipboard.snapshot?.id == result.clipboardId) clipboard.snapshot = null
    }
    val background = FileTransferBackground(application,this,scope)
    // This controller owns credentials for file tasks only. It creates no terminal/browser.
    private val authenticationOwner = lazy { SessionsViewModel(application,monitorSessions=false) }
    val authentication get() = authenticationOwner.value
    private val resumed = linkedSetOf<UUID>()
    var presenter by mutableStateOf<UUID?>(null); private set

    fun connections(request: FilePasteRequest): FileTaskConnections =
        authentication.fileTaskConnections(request) { tasks.cancelRequest(request.id) }
    var retainedReleaseError by mutableStateOf<String?>(null); private set
    internal fun releaseRetained(service: com.hanxi.ghosterm.data.LocalRetainedFiles, ref: com.hanxi.ghosterm.data.LocalRetainedFiles.Ref) {
        scope.launch {
            try { service.dismiss(ref) }
            catch(error:Exception) { retainedReleaseError="暂存查看资源释放失败：${error.message ?: "未知原因"}" }
        }
    }
    fun resume(id: UUID) { resumed.remove(id);resumed.add(id);presenter=id;background.refreshNotifications() }
    fun pause(id: UUID) { resumed.remove(id);if(presenter==id) presenter=resumed.lastOrNull() }
    fun focus(id: UUID) { if(id in resumed) { resumed.remove(id);resumed.add(id);presenter=id } }
    fun release(id: UUID) {
        pause(id)
        if(authenticationOwner.isInitialized()) authentication.releaseIdentityPresenter(id)
    }
}

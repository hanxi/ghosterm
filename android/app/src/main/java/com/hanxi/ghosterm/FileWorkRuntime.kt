package com.hanxi.ghosterm

import android.app.Application
import androidx.compose.runtime.*
import com.hanxi.ghosterm.data.FileTaskConnections
import com.hanxi.ghosterm.domain.FileClipboardSnapshot
import com.hanxi.ghosterm.domain.FilePasteRequest
import kotlinx.coroutines.*
import java.util.UUID

/** One in-memory file workspace per application process, without Activity references. */
class FileClipboardStore {
    var snapshot by mutableStateOf<FileClipboardSnapshot?>(null)
        internal set
}

class FileWorkRuntime(application: Application) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val clipboard = FileClipboardStore()
    val tasks = FileTaskStore(application,scope)
    // This controller owns credentials for file tasks only. It creates no terminal/browser.
    private val authenticationOwner = lazy { SessionsViewModel(application,monitorSessions=false) }
    val authentication get() = authenticationOwner.value
    private val resumed = linkedSetOf<UUID>()
    var presenter by mutableStateOf<UUID?>(null); private set

    fun connections(request: FilePasteRequest): FileTaskConnections =
        authentication.fileTaskConnections(request) { tasks.cancelRequest(request.id) }
    fun resume(id: UUID) { resumed.remove(id);resumed.add(id);presenter=id }
    fun pause(id: UUID) { resumed.remove(id);if(presenter==id) presenter=resumed.lastOrNull() }
    fun focus(id: UUID) { if(id in resumed) { resumed.remove(id);resumed.add(id);presenter=id } }
    fun release(id: UUID) {
        pause(id)
        if(authenticationOwner.isInitialized()) authentication.releaseIdentityPresenter(id)
    }
}

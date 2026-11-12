package com.hanxi.ghosterm

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/** Per-AppModel review state. Trust is deliberately process-local in this slice. */
class ExtensionPackagesViewModel(application: Application) : AndroidViewModel(application) {
    data class Entry(val review: ExtensionController.Review, val decision: String = "尚未批准")

    internal val controller: ExtensionController? = if (BuildConfig.PLUGIN_JS_ENABLED && ExtensionRuntime.supportStatus() == 0) {
        runCatching {
            ExtensionController(
                ExtensionController.HostProfile(
                    sdkVersion = "1.0.0", protocols = listOf(1), runtimes = listOf("portable-js"),
                    capabilities = listOf("session.rpc", "session.resources")),
                application.applicationContext)
        }.getOrNull()
    } else null

    val runtimeAvailable get() = controller != null
    var busy by mutableStateOf(false)
        private set
    var error: String? by mutableStateOf(null)
        private set
    var entries: List<Entry> by mutableStateOf(emptyList())
        private set

    fun reviewTree(uri: Uri) {
        val owner = controller ?: return
        if (busy) return
        busy = true
        error = null
        viewModelScope.launch {
            try {
                val review = owner.reviewTree(uri)
                if (entries.none { it.review.id == review.id && it.review.digest == review.digest }) {
                    entries = entries + Entry(review)
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "插件包审查失败：${failure.message ?: failure.javaClass.simpleName}"
            } finally {
                busy = false
            }
        }
    }

    fun decide(entry: Entry, approve: Boolean) {
        val owner = controller ?: return
        if (busy) return
        busy = true
        error = null
        viewModelScope.launch {
            try {
                val status = if (approve) owner.approve(entry.review.id, entry.review.digest)
                    else owner.deny(entry.review.id, entry.review.digest)
                val accepted = if (approve) status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK
                    else status == com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.DENIED
                if (!accepted) error = "来源裁决未完成：$status"
                else entries = entries.map {
                    if (it.review.id == entry.review.id && it.review.digest == entry.review.digest) {
                        it.copy(decision = if (approve) "已批准（本进程）" else "已拒绝")
                    } else it
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Throwable) {
                error = "来源裁决失败：${failure.message ?: failure.javaClass.simpleName}"
            } finally {
                busy = false
            }
        }
    }

}

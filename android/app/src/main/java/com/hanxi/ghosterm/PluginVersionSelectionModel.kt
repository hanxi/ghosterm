package com.hanxi.ghosterm

import android.net.Uri
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*
import java.util.UUID

/** Policy trust, version preview and explicit commit are separate decisions. */
internal class PluginVersionSelectionModel(
    private val scope: CoroutineScope,
    private val read: suspend (Uri) -> PluginSignatureDocument,
    private val inspect: suspend (String, String, String?, ByteArray) -> ExtensionController.VersionPreview,
    private val commit: suspend (String) -> ExtensionController.VersionSelectionResult,
    private val cancelRequest: (String) -> Unit,
    private val disable: suspend (String, String?) -> ExtensionController.VersionSelectionResult,
    private val changed: (ExtensionController.VersionSelectionResult) -> Unit,
    private val policyChanged: (PluginSignatureDocument?, Boolean) -> Unit = { _, _ -> },
) {
    class Pick internal constructor()
    data class Disable(val pluginID: String, val revision: String?)
    private var closed = false
    private var epoch = Any()
    private var requestID: String? = null
    private var operation: Job? = null
    var pending: Pick? by mutableStateOf(null); private set
    var policy: PluginSignatureDocument? by mutableStateOf(null); private set
    var trusted by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var committing by mutableStateOf(false); private set
    var preview: ExtensionController.VersionPreview? by mutableStateOf(null); private set
    var disableTarget: Disable? by mutableStateOf(null); private set
    var acknowledged by mutableStateOf(false); private set
    var issue: String? by mutableStateOf(null); private set
    var result: ExtensionController.VersionSelectionResult? by mutableStateOf(null); private set
    private val readyForReview get() = !closed && !busy && pending == null
    val canPreview get() = readyForReview && policy != null && trusted
    fun canPreview(requiresChannelPolicy: Boolean) = if (requiresChannelPolicy) readyForReview else canPreview
    val canConfirm get() = !closed && !busy && preview != null && acknowledged && (trusted || preview?.candidate?.channelStatementSHA256 != null)
    val canDisable get() = !closed && !busy && disableTarget != null && acknowledged
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    private fun invalidate() {
        epoch = Any(); operation?.cancel(); requestID?.let(cancelRequest); requestID = null
        preview = null; disableTarget = null; acknowledged = false; issue = null; result = null; busy = false
    }
    fun beginPick(): Pick? {
        main(); if (closed || busy || pending != null) return null
        invalidate(); policy = null; trusted = false; policyChanged(null, false)
        return Pick().also { pending = it }
    }
    fun failedPick(pick: Pick) {
        main(); if (!closed && pending === pick) { pending = null; issue = "无法打开策略文档选择器。" }
    }
    fun completePick(pick: Pick, uri: Uri?) {
        main(); if (closed || pending !== pick) return
        pending = null; if (uri == null) return
        val generation = epoch; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try { val value = read(uri); if (!closed && generation === epoch) { policy = value; policyChanged(value, trusted) } }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (!closed && generation === epoch) issue = message(failure, false) }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun trust(value: Boolean) {
        main(); if (closed || committing || value && policy == null) return
        invalidate(); trusted = value; policyChanged(policy, value)
    }
    fun inspect(releaseID: String, revision: String?, requiresChannelPolicy: Boolean = false) {
        main(); if (!canPreview(requiresChannelPolicy)) return
        invalidate()
        val id = UUID.randomUUID().toString(); requestID = id
        val generation = epoch; val bytes = if (requiresChannelPolicy) byteArrayOf() else policy!!.copyBytes(); busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = inspect(id, releaseID, revision, bytes)
                if (!closed && generation === epoch) preview = value else cancelRequest(value.id)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (!closed && generation === epoch) { cancelRequest(id); requestID = null; issue = message(failure, false) } }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun acknowledge(value: Boolean) {
        main(); if (!closed && !busy && (preview != null || disableTarget != null)) acknowledged = value
    }
    fun confirm() {
        main(); if (!canConfirm) return
        val id = preview!!.id; val generation = epoch
        busy = true; committing = true; issue = null
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = commit(id)
                if (!closed && generation === epoch) { result = value; preview = null; requestID = null; changed(value) }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) {
                if (!closed && generation === epoch) { cancelRequest(id); preview = null; requestID = null; issue = message(failure, true) }
            } finally { if (generation === epoch) { acknowledged = false; busy = false; committing = false } }
        }
    }
    fun beginDisable(pluginID: String, revision: String?) {
        main(); if (closed || busy) return
        invalidate(); disableTarget = Disable(pluginID, revision)
    }
    fun confirmDisable() {
        main(); if (!canDisable) return
        val target = disableTarget!!; val generation = epoch
        busy = true; committing = true; issue = null
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = disable(target.pluginID, target.revision)
                if (!closed && generation === epoch) { result = value; disableTarget = null; changed(value) }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (!closed && generation === epoch) { disableTarget = null; issue = message(failure, true) } }
            finally { if (generation === epoch) { acknowledged = false; busy = false; committing = false } }
        }
    }
    fun cancel() { main(); if (!closed && !committing) invalidate() }
    fun close() { main(); closed = true; invalidate(); pending = null; policy = null; trusted = false; committing = false; policyChanged(null, false) }
    private fun message(failure: Throwable, writing: Boolean): String = when (failure) {
        is PluginReleaseCatalog.Rejected, is PluginReleaseSignature.Rejected, is PluginReleaseDeclarations.Rejected,
        is PluginSignatureReadFailure -> failure.message ?: "材料或版本记录无效，请重新核对。"
        else -> if (writing) "未取得完整活动选择回执；请刷新列表核对当前选择。" else "无法完成版本核验，请刷新并重新选择材料。"
    }
}

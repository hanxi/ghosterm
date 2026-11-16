package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*
import java.util.UUID

/** Page-local tickets and unchecked subsets; no observation writes selection. */
internal class PluginPolicyReviewModel(private val scope: CoroutineScope,
    private val inspect: suspend (String, ByteArray) -> PluginPolicyReview.Value,
    private val commit: suspend (String, List<String>, ByteArray) -> ExtensionController.PolicyDisableResult,
    private val cancelRequest: (String) -> Unit,
    private val changed: (ExtensionController.PolicyDisableResult) -> Unit) {
    private var closed = false
    private var epoch = Any()
    private var operation: Job? = null
    private var requestID: String? = null
    private var policy: PluginSignatureDocument? = null
    private var trusted by mutableStateOf(false)
    var busy by mutableStateOf(false); private set
    var committing by mutableStateOf(false); private set
    var review: PluginPolicyReview.Value? by mutableStateOf(null); private set
    var selectedIDs: Set<String> by mutableStateOf(emptySet()); private set
    var issue: String? by mutableStateOf(null); private set
    var result: ExtensionController.PolicyDisableResult? by mutableStateOf(null); private set
    val canReview get() = !closed && !busy && policy != null && trusted
    val canApply get() = canReview && review != null && selectedIDs.isNotEmpty()
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    private fun invalidate() {
        epoch = Any(); operation?.cancel(); operation = null
        requestID?.let(cancelRequest); requestID = null
        review = null; selectedIDs = emptySet(); issue = null; result = null; busy = false
    }
    fun setPolicy(document: PluginSignatureDocument?, trust: Boolean) {
        main(); if (closed || committing) return
        invalidate(); policy = document; trusted = document != null && trust
    }
    fun inspect() {
        main(); if (!canReview) return
        invalidate(); val id = UUID.randomUUID().toString(); requestID = id
        val bytes = policy!!.copyBytes(); val generation = epoch; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = inspect(id, bytes)
                if (!closed && generation === epoch && trusted) review = value else cancelRequest(value.id)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) {
                if (!closed && generation === epoch) { cancelRequest(id); requestID = null; issue = message(failure, false) }
            } finally { if (generation === epoch) busy = false }
        }
    }
    fun choose(id: String, enabled: Boolean) {
        main(); if (closed || busy || review?.entries?.any { it.pluginID == id && it.state != PluginPolicyReview.State.ACCEPTED } != true) return
        selectedIDs = if (enabled) selectedIDs + id else selectedIDs - id
    }
    fun apply() {
        main(); if (!canApply) return
        val id = review!!.id; val ids = selectedIDs.sorted(); val bytes = policy!!.copyBytes(); val generation = epoch
        busy = true; committing = true; issue = null
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = commit(id, ids, bytes)
                if (!closed && generation === epoch) { result = value; review = null; requestID = null; selectedIDs = emptySet(); changed(value) }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) {
                if (!closed && generation === epoch) { cancelRequest(id); requestID = null; review = null; selectedIDs = emptySet(); issue = message(failure, true) }
            } finally { if (generation === epoch) { busy = false; committing = false } }
        }
    }
    fun cancel() { main(); if (!closed && !committing) invalidate() }
    fun close() { main(); closed = true; invalidate(); policy = null; trusted = false; committing = false }
    private fun message(failure: Exception, writing: Boolean) = when (failure) {
        is PluginReleaseSignature.Rejected, is PluginReleaseCatalog.Rejected -> failure.message ?: "策略或选择状态查不出来。"
        else -> if (writing) "未取得完整禁用回执；请刷新版本列表核对当前选择。" else "无法完成策略复核；未修改活动选择。"
    }
}

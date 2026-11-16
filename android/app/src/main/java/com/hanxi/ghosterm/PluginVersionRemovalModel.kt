package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*
import java.util.UUID

/** Every disk mutation requires an explicit, separately acknowledged action.
 * Observation is read-only, including after an interrupted confirmation. */
internal class PluginVersionRemovalModel(
    private val scope: CoroutineScope,
    private val observe: suspend () -> PluginReleaseRemoval.Preview?,
    private val inspect: suspend (String, String, String?) -> PluginReleaseRemoval.Preview,
    private val commit: suspend (String) -> PluginReleaseRemoval.Result,
    private val cancelPreview: (String) -> Unit,
    private val resume: suspend (String) -> PluginReleaseRemoval.Result,
    private val withdraw: suspend (String) -> Boolean,
    private val changed: (PluginReleaseRemoval.Result?) -> Unit,
    private val inspectStageRequest: (suspend (String, PluginInstallJournal.Reference) -> PluginReleaseRemoval.Preview)? = null,
    private val inspectUnclassifiedRequest: (suspend (String, PluginUnclassifiedDirectories.Reference) -> PluginReleaseRemoval.Preview)? = null,
    private val commitUnclassified: (suspend (String, Boolean) -> PluginReleaseRemoval.Result)? = null,
    private val resumeUnclassified: (suspend (String, Boolean) -> PluginReleaseRemoval.Result)? = null,
) {
    private var closed = false
    private var epoch = Any()
    private var operation: Job? = null
    private var requestID: String? = null
    var loaded by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var committing by mutableStateOf(false); private set
    var preview: PluginReleaseRemoval.Preview? by mutableStateOf(null); private set
    var saved: PluginReleaseRemoval.Preview? by mutableStateOf(null); private set
    var acknowledged by mutableStateOf(false); private set
    var acknowledgedUnclassified by mutableStateOf(false); private set
    var issue: String? by mutableStateOf(null); private set
    var result: PluginReleaseRemoval.Result? by mutableStateOf(null); private set
    var withdrawalDurability: Boolean? by mutableStateOf(null); private set
    val canPreview get() = !closed && !busy && loaded && saved == null && issue == null
    val canConfirm get() = !closed && !busy && preview != null && acknowledged && (preview?.unclassified == null || acknowledgedUnclassified)
    val canContinue get() = !closed && !busy && saved != null && acknowledged && (saved?.unclassified == null || acknowledgedUnclassified)
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    private fun invalidate() {
        epoch = Any(); operation?.cancel(); operation = null
        requestID?.let(cancelPreview); requestID = null; preview = null; acknowledged = false; acknowledgedUnclassified = false
        issue = null; result = null; withdrawalDurability = null; busy = false
    }
    fun refresh() {
        main(); if (closed || committing) return
        invalidate(); loaded = false; saved = null
        val generation = epoch; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try { val value = observe(); if (!closed && generation === epoch) { saved = value; loaded = true } }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (!closed && generation === epoch) issue = message(failure) }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun inspect(releaseID: String, revision: String?) {
        main(); if (!canPreview) return
        beginInspect { id -> inspect(id, releaseID, revision) }
    }
    fun inspectStage(reference: PluginInstallJournal.Reference) {
        main(); if (!canPreview || inspectStageRequest == null) return
        beginInspect { id -> inspectStageRequest.invoke(id, reference) }
    }
    fun inspectUnclassified(reference: PluginUnclassifiedDirectories.Reference) {
        main(); if (!canPreview || inspectUnclassifiedRequest == null) return
        beginInspect { id -> inspectUnclassifiedRequest.invoke(id, reference) }
    }
    fun acknowledgeUnclassified(value: Boolean) {
        main(); if (!closed && !busy && (preview?.unclassified != null || saved?.unclassified != null)) acknowledgedUnclassified = value
    }
    private fun beginInspect(action: suspend (String) -> PluginReleaseRemoval.Preview) {
        invalidate(); val id = UUID.randomUUID().toString(); requestID = id
        val generation = epoch; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = action(id)
                if (!closed && generation === epoch) preview = value else cancelPreview(value.id)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (!closed && generation === epoch) { cancelPreview(id); requestID = null; issue = message(failure) } }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun acknowledge(value: Boolean) { main(); if (!closed && !busy && (preview != null || saved != null)) acknowledged = value }
    fun confirm() { main(); if (canConfirm) mutate(preview!!.id, 0) }
    fun resume() { main(); if (canContinue) mutate(saved!!.id, 1) }
    fun withdraw() { main(); if (canContinue) mutate(saved!!.id, 2) }
    private fun mutate(id: String, action: Int) {
        val generation = epoch; val unknown = (preview ?: saved)?.unclassified != null; val unknownAck = acknowledgedUnclassified; busy = true; committing = true; issue = null
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val value = when (action) {
                    0 -> if (unknown) checkNotNull(commitUnclassified).invoke(id, unknownAck) else commit(id)
                    1 -> if (unknown) checkNotNull(resumeUnclassified).invoke(id, unknownAck) else resume(id)
                    else -> null
                }
                val durable = if (action == 2) withdraw(id) else null
                if (!closed && generation === epoch) {
                    result = value; withdrawalDurability = durable; preview = null; requestID = null
                    changed(value)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) {
                if (!closed && generation === epoch) { issue = message(failure); cancelPreview(id); preview = null; requestID = null }
            } finally {
                if (!closed && generation === epoch) {
                    // A missing receipt does not mean nothing happened. Re-read
                    // only; never replay or infer absence from an IO error.
                    try { saved = observe(); loaded = true }
                    catch (_: Exception) { saved = null; loaded = false; issue = issue ?: "待处理删除意图查不出来；请刷新核对。" }
                    acknowledged = false; acknowledgedUnclassified = false; busy = false; committing = false
                }
            }
        }
    }
    fun cancel() { main(); if (!closed && !committing) invalidate() }
    fun close() { main(); closed = true; invalidate(); saved = null; loaded = false; committing = false }
    private fun message(failure: Exception) = when (failure) {
        is PluginReleaseRemoval.Rejected, is PluginReleaseCatalog.Rejected -> failure.message ?: "回收状态查不出来，请刷新核对。"
        else -> "未取得完整回收回执；请刷新核对目录和保留的删除意图。"
    }
}

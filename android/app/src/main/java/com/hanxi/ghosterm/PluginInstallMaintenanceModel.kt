package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*

/** Observation and explicit metadata retirement are separate operations. Tree
 * removal belongs to the full-inventory removal model, never this list. */
internal class PluginInstallMaintenanceModel(
    private val scope: CoroutineScope,
    private val read: suspend () -> PluginInstallJournal.Observation,
    private val retire: suspend (PluginInstallJournal.Reference) -> Boolean,
    private val changed: () -> Unit,
) {
    private var closed = false
    private var epoch = Any()
    private var operation: Job? = null
    var observation: PluginInstallJournal.Observation? by mutableStateOf(null); private set
    var issue: String? by mutableStateOf(null); private set
    var busy by mutableStateOf(false); private set
    var committing by mutableStateOf(false); private set
    var target: PluginInstallJournal.Reference? by mutableStateOf(null); private set
    var acknowledged by mutableStateOf(false); private set
    var result: Boolean? by mutableStateOf(null); private set
    val canConfirm get() = !closed && !busy && target != null && acknowledged
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    private fun invalidate() { epoch = Any(); operation?.cancel(); operation = null; target = null; acknowledged = false; issue = null; busy = false; result = null }
    fun refresh() {
        main(); if (closed || committing) return
        invalidate(); observation = null; val generation = epoch; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try { val value = read(); if (!closed && generation === epoch) observation = value }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (!closed && generation === epoch) issue = "安装归属记录查不出来，未当作空列表。" }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun choose(entry: PluginInstallJournal.Entry) {
        main(); if (closed || busy || entry.reference == null || entry.state !in setOf(PluginInstallJournal.State.PUBLISHED, PluginInstallJournal.State.ABSENT)) return
        if (observation?.entries?.contains(entry) != true) return
        invalidate(); target = entry.reference
    }
    fun acknowledge(value: Boolean) { main(); if (!closed && !busy && target != null) acknowledged = value }
    fun confirm() {
        main(); if (!canConfirm) return
        val reference = target!!; val generation = epoch; busy = true; committing = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try {
                val durable = retire(reference)
                if (!closed && generation === epoch) { result = durable; target = null; changed() }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (!closed && generation === epoch) { target = null; issue = "未取得归属记录退役回执；请刷新核对，未删除其它对象。" } }
            finally {
                if (!closed && generation === epoch) {
                    try { observation = read() } catch (_: Exception) { observation = null; issue = issue ?: "安装归属记录查不出来。" }
                    acknowledged = false; busy = false; committing = false
                }
            }
        }
    }
    fun cancel() { main(); if (!closed && !committing) invalidate() }
    fun close() { main(); closed = true; invalidate(); committing = false; observation = null }
}

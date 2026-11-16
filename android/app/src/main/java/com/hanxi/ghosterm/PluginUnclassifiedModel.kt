package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*

internal class PluginUnclassifiedModel(private val scope: CoroutineScope,
    private val read: suspend () -> List<PluginUnclassifiedDirectories.Entry>) {
    private var closed = false
    private var epoch = Any()
    private var operation: Job? = null
    var entries: List<PluginUnclassifiedDirectories.Entry>? by mutableStateOf(null); private set
    var issue: String? by mutableStateOf(null); private set
    var busy by mutableStateOf(false); private set
    fun refresh() {
        check(Looper.myLooper() == Looper.getMainLooper()); if (closed || busy) return
        operation?.cancel(); epoch = Any(); val generation = epoch; entries = null; issue = null; busy = true
        operation = scope.launch(Dispatchers.Main.immediate) {
            try { val value = read(); if (!closed && generation === epoch) entries = value }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (!closed && generation === epoch) issue = "未知目录查不出来；未当作空列表或授予删除权限。" }
            finally { if (generation === epoch) busy = false }
        }
    }
    fun close() {
        check(Looper.myLooper() == Looper.getMainLooper()); closed = true; epoch = Any(); operation?.cancel(); entries = null; busy = false
    }
}

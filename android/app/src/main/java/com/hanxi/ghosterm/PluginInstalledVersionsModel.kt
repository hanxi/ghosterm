package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/** One explicit inventory request; no filesystem writes or startup replay. */
internal class PluginInstalledVersionsModel(private val scope: CoroutineScope,
    private val load: suspend () -> PluginReleaseCatalog.Observation) {
    private var closed = false
    private var generation = Any()
    private var request: Job? = null
    var busy by mutableStateOf(false); private set
    var observation: PluginReleaseCatalog.Observation? by mutableStateOf(null); private set
    var issue: String? by mutableStateOf(null); private set
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    fun refresh() {
        main()
        if (closed || busy) return
        val selected = Any(); generation = selected
        busy = true; observation = null; issue = null
        request = scope.launch {
            try {
                val value = load()
                if (!closed && generation === selected) observation = value
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) {
                if (!closed && generation === selected) issue = if (failure is PluginReleaseCatalog.Rejected) failure.message
                    else "无法读取已保存版本；当前状态查不出来。"
            } finally { busy = false }
        }
    }
    fun close() {
        main(); closed = true; generation = Any(); request?.cancel(); observation = null; issue = null
    }
}

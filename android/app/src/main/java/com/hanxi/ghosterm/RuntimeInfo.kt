package com.hanxi.ghosterm

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.hanxi.ghosterm.bridge.CoreLibrary
import com.hanxi.ghosterm.terminal.TerminalEngine
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface RuntimeInfo {
    data object Loading : RuntimeInfo
    data class Ready(val core: String, val terminal: String) : RuntimeInfo
    data object Failed : RuntimeInfo
}

/** Loads libraries off the UI thread. Loading failure never becomes "connected". */
class RuntimeInfoViewModel : ViewModel() {
    private val mutable = MutableStateFlow<RuntimeInfo>(RuntimeInfo.Loading)
    val state = mutable.asStateFlow()
    init {
        viewModelScope.launch {
            mutable.value = withContext(Dispatchers.IO) {
                try { RuntimeInfo.Ready(CoreLibrary.version, TerminalEngine.version) }
                catch (_: UnsatisfiedLinkError) { RuntimeInfo.Failed }
                catch (_: IllegalStateException) { RuntimeInfo.Failed }
            }
        }
    }
}

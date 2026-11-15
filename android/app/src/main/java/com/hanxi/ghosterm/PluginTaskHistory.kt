package com.hanxi.ghosterm

/** Runtime-local observations survive a widget close. Full history refuses new
 * scopes rather than discarding a possibly unknown cancellation. */
internal class PluginTaskHistory {
    data class Record(val descriptor: PluginTaskDescriptor, var snapshot: PluginTaskSnapshot? = null,
        var requestID: String? = null, var cancellation: PluginTaskModel.Cancellation = PluginTaskModel.Cancellation.IDLE)
    class Full : IllegalStateException("本次任务观察记录已满，请停止并释放插件后再试。")
    class Changed : IllegalStateException("同一任务修订的身份描述已改变，未显示新状态。")
    private val records = mutableMapOf<Pair<String, String>, Record>()
    private fun key(d: PluginTaskDescriptor) = d.viewID to d.revision
    fun restore(d: PluginTaskDescriptor): Record {
        records[key(d)]?.let { if (it.descriptor != d) throw Changed(); return it.copy() }
        if (records.size >= 8) throw Full()
        return Record(d).also { records[key(d)] = it }.copy()
    }
    fun observe(d: PluginTaskDescriptor, snapshot: PluginTaskSnapshot) { records[key(d)]?.snapshot = snapshot }
    fun action(d: PluginTaskDescriptor, id: String, state: PluginTaskModel.Cancellation) {
        records[key(d)]?.let { if (it.requestID == null || it.requestID == id) { it.requestID = id; it.cancellation = state } }
    }
    val retained get() = records.size
}

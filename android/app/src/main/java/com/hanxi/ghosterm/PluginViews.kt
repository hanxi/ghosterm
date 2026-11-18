package com.hanxi.ghosterm

import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

internal data class PluginViewRegistration(val handle: String, val viewID: String, val kind: Kind,
    val describe: String, val read: String, val cancel: String?) {
    enum class Kind(val wire: String) { TABLE("table"), DIFF("diff"), TASK("task") }
    val methods get() = listOfNotNull(describe, read, cancel).toSet()
    val counter get() = handle.substringAfter('.').toULong()
    val nonce get() = handle.substringBefore('.')
}

internal data class PluginViewRegistrySnapshot(val instance: String, val epoch: String, val revision: ULong,
    val entries: List<PluginViewRegistration>) {
    companion object {
        fun parse(bytes: ByteArray): PluginViewRegistrySnapshot = with(PluginViewValue) {
            val root = envelope(bytes, 8192); root.keys(setOf("instanceId", "epoch", "revision", "views"))
            fun hex(value: String) = value.matches(Regex("[0-9a-f]{32}"))
            val instance = root["instanceId"].text(32, true); require(hex(instance))
            val entries = root["views"].items(); require(entries.size <= 8)
            val parsed = entries.map { value ->
                val entry = value.fields(); entry.keys(setOf("handle", "viewId", "kind", "methods"))
                val handle = entry["handle"].text(64, true)
                require(handle.count { it == '.' } == 1 && hex(handle.substringBefore('.')))
                PluginViewValue.Value.Text(handle.substringAfter('.')).revision()
                val kind = PluginViewRegistration.Kind.entries.firstOrNull { it.wire == entry["kind"].text(5) } ?: error("视图类型无效")
                val methods = entry["methods"].fields()
                methods.keys(if (kind == PluginViewRegistration.Kind.TASK) setOf("describe", "read", "cancel") else setOf("describe", "read"))
                fun method(name: String) = methods[name].text(128, true).also { require(it.matches(Regex("[a-z][A-Za-z0-9._-]{0,127}"))) }
                val r = PluginViewRegistration(handle, entry["viewId"].id(), kind, method("describe"), method("read"),
                    if (kind == PluginViewRegistration.Kind.TASK) method("cancel") else null)
                require(r.methods.size == methods.size); r
            }
            require(parsed.map { it.handle }.distinct().size == parsed.size && parsed.map { it.viewID }.distinct().size == parsed.size &&
                parsed.map { it.nonce }.distinct().size <= 1)
            PluginViewRegistrySnapshot(instance, root["epoch"].revision(), root["revision"].revision().toULong(), captured(parsed))
        }
    }
}

internal class PluginViewItem(val registration: PluginViewRegistration, scope: CoroutineScope,
    request: suspend (String, ByteArray) -> ByteArray, history: PluginTaskHistory, isCurrent: () -> Boolean) {
    sealed interface Content {
        data class Table(val model: PluginTableModel) : Content
        data class Diff(val model: PluginDiffModel) : Content
        data class Task(val model: PluginTaskModel) : Content
    }
    val handle get() = registration.handle
    var closing by mutableStateOf(false); private set
    var closeIssue: String? by mutableStateOf(null); private set
    private var live by mutableStateOf(true)
    private val available = { live && !closing && isCurrent() }
    private val mapped: suspend (String, ByteArray) -> ByteArray = { method, input ->
        check(available()) { "视图已失效" }
        val name = when (method) {
            "view.table.describe", "view.diff.describe", "view.task.describe" -> registration.describe
            "view.table.page", "view.diff.page", "view.task.read" -> registration.read
            "view.task.cancel" -> checkNotNull(registration.cancel)
            else -> error("视图方法无效")
        }
        request(name, input)
    }
    val content = when (registration.kind) {
        PluginViewRegistration.Kind.TABLE -> Content.Table(PluginTableModel(scope, mapped, isCurrent = available, expectedViewID = registration.viewID))
        PluginViewRegistration.Kind.DIFF -> Content.Diff(PluginDiffModel(scope, mapped, isCurrent = available, expectedViewID = registration.viewID))
        PluginViewRegistration.Kind.TASK -> Content.Task(PluginTaskModel(scope, mapped, isCurrent = available, expectedViewID = registration.viewID, history = history))
    }
    fun beginClose() { closing = true; closeIssue = null }
    fun closeFailed() { closing = false; closeIssue = "视图关闭未确认，可显式重试。任务取消仍需单独操作。" }
    fun invalidate() {
        live = false
        when (val c = content) { is Content.Table -> c.model.invalidate(); is Content.Diff -> c.model.invalidate(); is Content.Task -> c.model.invalidate() }
    }
}

/** Eight active components, one nonce/counter high-water mark and eight retained
 * task scopes. No unbounded handle tombstones or silent history eviction. */
internal class PluginViewsModel(private val scope: CoroutineScope, private val instance: String, private val epoch: String,
    kinds: Set<PluginViewRegistration.Kind>, capabilities: Set<String>, methods: Set<String>,
    private val request: suspend (String, ByteArray) -> ByteArray, private val isCurrent: () -> Boolean,
    private val close: suspend (String) -> ByteArray) {
    data class State(val items: List<PluginViewItem> = emptyList(), val valid: Boolean = true, val issue: String? = null)
    private val kinds = kinds.toSet(); private val methods = methods.toSet()
    private val mutable = MutableStateFlow(State()); val state = mutable.asStateFlow()
    private var last: PluginViewRegistrySnapshot? = null
    private var nonce: String? = null
    private var highWater = 0uL
    private val history = PluginTaskHistory()
    init {
        require(kinds.isNotEmpty() && capabilities.containsAll(setOf("session.rpc", "ui.views")) &&
            kinds.all { "ui.${it.wire}" in capabilities })
    }
    val available get() = state.value.valid && scope.isActive && isCurrent()
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Views require Main owner" } }
    fun apply(bytes: ByteArray) {
        main(); if (!available) return
        try {
            val snapshot = PluginViewRegistrySnapshot.parse(bytes)
            require(snapshot.instance == instance && snapshot.epoch == epoch)
            last?.let {
                if (snapshot.revision < it.revision) return // queued before a confirmed Native close
                if (snapshot.revision == it.revision) { require(snapshot == it); return }
            }
            val prior = state.value.items.associateBy { it.handle }
            snapshot.entries.forEach { r ->
                require(r.kind in kinds && methods.containsAll(r.methods) && (nonce == null || nonce == r.nonce))
                val old = prior[r.handle]
                if (old != null) require(old.registration == r) else require(r.counter > highWater)
            }
            val selected = snapshot.entries.map { r -> prior[r.handle] ?: PluginViewItem(r, scope, request, history) { available } }
            val handles = snapshot.entries.map { it.handle }.toSet()
            state.value.items.filter { it.handle !in handles }.forEach { it.invalidate() }
            highWater = maxOf(highWater, snapshot.entries.maxOfOrNull { it.counter } ?: 0uL)
            if (nonce == null) nonce = snapshot.entries.firstOrNull()?.nonce
            mutable.value = state.value.copy(items = PluginViewValue.captured(selected)); last = snapshot
        } catch (failure: Exception) {
            invalidate(); mutable.value = state.value.copy(issue = "视图注册快照无效，已停用本次视图。可停止并释放插件。")
            throw failure
        }
    }
    fun closeView(handle: String): kotlinx.coroutines.Job? {
        main(); if (!available) return null
        val item = state.value.items.firstOrNull { it.handle == handle && !it.closing } ?: return null
        item.beginClose()
        return scope.launch(start = CoroutineStart.UNDISPATCHED) {
            try {
                val bytes = close(handle)
                if (!available || state.value.items.none { it === item }) return@launch
                apply(bytes)
                if (state.value.items.any { it === item }) item.closeFailed()
            } catch (cancelled: CancellationException) {
                if (available && state.value.items.any { it === item }) item.closeFailed()
                throw cancelled
            } catch (_: Exception) { if (available && state.value.items.any { it === item }) item.closeFailed() }
        }
    }
    fun invalidate() {
        main()
        val current = state.value
        if (!current.valid) return
        current.items.forEach { it.invalidate() }
        mutable.value = current.copy(items = emptyList(), valid = false)
    }
}

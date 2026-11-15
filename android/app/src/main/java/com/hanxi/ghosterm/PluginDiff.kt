package com.hanxi.ghosterm

import android.os.Looper
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject

internal data class PluginDiffDescriptor(val viewID: String, val revision: String, val title: String,
    val beforeLabel: String, val afterLabel: String, val beforeLines: Int, val afterLines: Int,
    val added: Int, val removed: Int, val total: Int, val pageSize: Int)
internal data class PluginDiffCursor(val before: Int, val after: Int) {
    companion object { val ZERO = PluginDiffCursor(0, 0) }
}
internal data class PluginDiffRow(val kind: Kind, val beforeLine: Int, val afterLine: Int, val text: String, val ending: Ending) {
    enum class Kind(val wire: String) { EQUAL("equal"), DELETE("delete"), INSERT("insert") }
    enum class Ending(val wire: String, val label: String) {
        LF("lf", "LF"), CRLF("crlf", "CRLF"), CR("cr", "CR"), NONE("none", "无末尾换行")
    }
}
internal data class PluginDiffPage(val offset: Int, val start: PluginDiffCursor, val end: PluginDiffCursor, val rows: List<PluginDiffRow>)

/** Standard read-only data, with exact source coordinates and trusted prefix. */
internal object PluginDiffCodec {
    fun descriptor(outcome: ByteArray): PluginDiffDescriptor = with(PluginViewValue) {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "title", "beforeLabel", "afterLabel", "beforeLines", "afterLines", "added", "removed", "total", "pageSize"))
        val before = value["beforeLines"].integer(0..128); val after = value["afterLines"].integer(0..128)
        val added = value["added"].integer(0..128); val removed = value["removed"].integer(0..128)
        val total = value["total"].integer(0..256)
        require(total - added == before && total - removed == after && added <= after && removed <= before) { "差异计数不匹配" }
        PluginDiffDescriptor(value["viewId"].id(), value["revision"].revision(), value["title"].text(128, true),
            value["beforeLabel"].text(64, true), value["afterLabel"].text(64, true), before, after, added, removed, total,
            value["pageSize"].integer(1..3))
    }
    fun page(outcome: ByteArray, description: PluginDiffDescriptor, offset: Int, prefix: PluginDiffCursor): PluginDiffPage = with(PluginViewValue) {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "offset", "total", "beforeOffset", "afterOffset", "rows"))
        require(value["viewId"].id() == description.viewID && value["revision"].revision() == description.revision) { "差异身份或修订已变化" }
        require(value["offset"].integer(0..256) == offset && value["total"].integer(0..256) == description.total && offset in 0..description.total) { "差异页面不匹配" }
        require(value["beforeOffset"].integer(0..128) == prefix.before && value["afterOffset"].integer(0..128) == prefix.after &&
            prefix.before <= description.beforeLines && prefix.after <= description.afterLines &&
            prefix.before + prefix.after in offset..2 * offset) { "差异游标不连续" }
        val rows = value["rows"].items()
        require(rows.size <= 3 && rows.size == minOf(description.pageSize, description.total - offset)) { "差异页行数不完整" }
        var before = prefix.before; var after = prefix.after
        val parsed = rows.map {
            val row = it.fields(); row.keys(setOf("kind", "beforeLine", "afterLine", "text", "ending"))
            val kind = PluginDiffRow.Kind.entries.firstOrNull { k -> k.wire == row["kind"].text(8, true) } ?: error("差异行类型无效")
            val ending = PluginDiffRow.Ending.entries.firstOrNull { e -> e.wire == row["ending"].text(4, true) } ?: error("差异换行无效")
            val old = row["beforeLine"].integer(0..128); val new = row["afterLine"].integer(0..128)
            if (kind != PluginDiffRow.Kind.INSERT) before++
            if (kind != PluginDiffRow.Kind.DELETE) after++
            val text = row["text"].text(128)
            require(old == (if (kind == PluginDiffRow.Kind.INSERT) 0 else before) && new == (if (kind == PluginDiffRow.Kind.DELETE) 0 else after) &&
                before <= description.beforeLines && after <= description.afterLines && '\r' !in text && '\n' !in text) { "差异行号或原文无效" }
            require(ending != PluginDiffRow.Ending.NONE || ((kind == PluginDiffRow.Kind.INSERT || before == description.beforeLines) &&
                (kind == PluginDiffRow.Kind.DELETE || after == description.afterLines))) { "无末尾换行只能出现在来源末行" }
            PluginDiffRow(kind, old, new, text, ending)
        }
        require(offset + parsed.size != description.total || (before == description.beforeLines && after == description.afterLines)) { "差异末页未覆盖两份原文" }
        PluginDiffPage(offset, prefix, PluginDiffCursor(before, after), captured(parsed))
    }
}

/** One selected page and bounded cursor history; no retained prior row content. */
internal class PluginDiffModel(private val scope: CoroutineScope,
    private val request: suspend (String, ByteArray) -> ByteArray, private val isCurrent: () -> Boolean = { true }) {
    companion object { val methods = listOf("view.diff.describe", "view.diff.page") }
    data class State(val descriptor: PluginDiffDescriptor? = null, val page: PluginDiffPage? = null,
        val selectedOffset: Int = 0, val busy: Boolean = false, val issue: String? = null, val valid: Boolean = true)
    private val mutable = MutableStateFlow(State())
    val state = mutable.asStateFlow()
    private var generation = Any()
    private val cursors = mutableMapOf(0 to PluginDiffCursor.ZERO)
    val available get() = state.value.valid && isCurrent()
    val canPrevious get() = available && !state.value.busy && state.value.page != null && state.value.selectedOffset > 0
    val canNext get() = available && !state.value.busy && state.value.page?.let { it.offset + it.rows.size < (state.value.descriptor?.total ?: 0) } == true
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Diff requires Main owner" } }
    fun load(): Job? {
        main(); if (!available || state.value.busy) return null
        val expected = Any(); generation = expected; cursors.clear(); cursors[0] = PluginDiffCursor.ZERO
        mutable.value = State(busy = true)
        return scope.launch {
            try {
                val bytes = request(methods[0], "{}".toByteArray())
                if (current(expected)) {
                    val description = PluginDiffCodec.descriptor(bytes)
                    mutable.value = state.value.copy(descriptor = description)
                    readPage(description, 0, PluginDiffCursor.ZERO, expected)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "差异读取失败，未显示新页面：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun showPage(offset: Int): Job? {
        main(); val description = state.value.descriptor ?: return null
        val prefix = cursors[offset] ?: return null
        if (!available || state.value.busy || offset !in 0 until maxOf(1, description.total) || offset % description.pageSize != 0) return null
        val expected = Any(); generation = expected
        mutable.value = state.value.copy(selectedOffset = offset, page = null, busy = true, issue = null)
        return scope.launch {
            try { readPage(description, offset, prefix, expected) }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "差异读取失败，未显示新页面：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun invalidate() {
        main(); generation = Any(); cursors.clear()
        mutable.value = state.value.copy(valid = false, busy = false, page = null, issue = "差异所属会话已结束")
    }
    private suspend fun current(expected: Any) = generation === expected && available && currentCoroutineContext().isActive
    private suspend fun readPage(description: PluginDiffDescriptor, offset: Int, prefix: PluginDiffCursor, expected: Any) {
        val input = JSONObject().put("viewId", description.viewID).put("revision", description.revision)
            .put("offset", offset).put("limit", description.pageSize).toString().toByteArray(Charsets.UTF_8)
        val bytes = request(methods[1], input)
        if (current(expected)) {
            val page = PluginDiffCodec.page(bytes, description, offset, prefix)
            cursors[offset + page.rows.size] = page.end
            mutable.value = state.value.copy(page = page)
        }
    }
}

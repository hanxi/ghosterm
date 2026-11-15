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

internal data class PluginTableColumn(val id: String, val title: String)
internal data class PluginTableDescriptor(val viewID: String, val revision: String, val title: String,
    val columns: List<PluginTableColumn>, val total: Int, val pageSize: Int)
internal data class PluginTableRow(val id: String, val cells: List<String>)
internal data class PluginTablePage(val offset: Int, val rows: List<PluginTableRow>)

/** A bounded standard table value reader, independent of any AI backend or UI. */
internal object PluginTableCodec {
    fun descriptor(outcome: ByteArray): PluginTableDescriptor = with(PluginViewValue) {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "title", "columns", "total", "pageSize"))
        val columns = value["columns"].items().also { require(it.size in 1..8) }.map {
            val column = it.fields(); column.keys(setOf("id", "title"))
            PluginTableColumn(column["id"].id(), column["title"].text(64, true))
        }
        require(columns.map { it.id }.distinct().size == columns.size) { "表格列 ID 重复" }
        PluginTableDescriptor(value["viewId"].id(), value["revision"].revision(), value["title"].text(128, true),
            captured(columns), value["total"].integer(0..1000000), value["pageSize"].integer(1..16))
    }
    fun page(outcome: ByteArray, descriptor: PluginTableDescriptor, offset: Int): PluginTablePage = with(PluginViewValue) {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "offset", "total", "rows"))
        require(value["viewId"].id() == descriptor.viewID && value["revision"].revision() == descriptor.revision) { "表格身份或修订已变化" }
        require(value["offset"].integer(0..1000000) == offset && value["total"].integer(0..1000000) == descriptor.total && offset in 0..descriptor.total) { "表格页面不匹配" }
        val rows = value["rows"].items()
        require(rows.size <= 16 && rows.size == minOf(descriptor.pageSize, descriptor.total - offset)) { "表格页行数不完整" }
        val parsed = rows.map {
            val row = it.fields(); row.keys(setOf("id", "cells"))
            val cells = row["cells"].items()
            require(cells.size == descriptor.columns.size) { "表格行列数不匹配" }
            PluginTableRow(row["id"].id(), captured(cells.map { cell -> cell.text(256) }))
        }
        require(parsed.map { it.id }.distinct().size == parsed.size) { "表格行 ID 重复" }
        PluginTablePage(offset, captured(parsed))
    }
    fun utf8(value: String): ByteArray = PluginViewValue.utf8(value)
}

/** One Main owner and one read at a time, bound to a captured Runtime scope. */
internal class PluginTableModel(private val scope: CoroutineScope,
    private val request: suspend (String, ByteArray) -> ByteArray, private val expectedViewID: String? = null,
    private val isCurrent: () -> Boolean = { true }) {
    companion object { val methods = listOf("view.table.describe", "view.table.page") }
    data class State(val descriptor: PluginTableDescriptor? = null, val page: PluginTablePage? = null,
        val selectedOffset: Int = 0, val busy: Boolean = false, val issue: String? = null, val valid: Boolean = true)
    private val mutable = MutableStateFlow(State())
    val state = mutable.asStateFlow()
    private var generation = Any()
    val available get() = state.value.valid && isCurrent()
    val lastOffset get() = state.value.descriptor?.let { if (it.total == 0) 0 else (it.total - 1) / it.pageSize * it.pageSize } ?: 0
    val canPrevious get() = available && !state.value.busy && state.value.page != null && state.value.selectedOffset > 0
    val canNext get() = available && !state.value.busy && state.value.page != null && state.value.selectedOffset < lastOffset
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) { "Table requires Main owner" } }
    fun load(): Job? {
        main(); if (!available || state.value.busy) return null
        val expected = Any(); generation = expected
        mutable.value = State(busy = true)
        return scope.launch {
            try {
                val bytes = request(methods[0], "{}".toByteArray())
                if (current(expected)) {
                    val description = PluginTableCodec.descriptor(bytes)
                    require(expectedViewID == null || expectedViewID == description.viewID) { "视图描述符 ID 与注册不一致" }
                    mutable.value = state.value.copy(descriptor = description)
                    readPage(description, 0, expected)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "表格读取失败，未显示新页面：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun showPage(offset: Int): Job? {
        main(); val description = state.value.descriptor ?: return null
        if (!available || state.value.busy || offset !in 0..lastOffset || offset % description.pageSize != 0) return null
        val expected = Any(); generation = expected
        mutable.value = state.value.copy(selectedOffset = offset, page = null, busy = true, issue = null)
        return scope.launch {
            try { readPage(description, offset, expected) }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "表格读取失败，未显示新页面：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun invalidate() {
        main(); generation = Any()
        mutable.value = state.value.copy(valid = false, busy = false, page = null, issue = "表格所属会话已结束")
    }
    private suspend fun current(expected: Any) = generation === expected && available && currentCoroutineContext().isActive
    private suspend fun readPage(description: PluginTableDescriptor, offset: Int, expected: Any) {
        val input = JSONObject().put("viewId", description.viewID).put("revision", description.revision)
            .put("offset", offset).put("limit", description.pageSize).toString().toByteArray(Charsets.UTF_8)
        val bytes = request(methods[1], input)
        if (current(expected)) mutable.value = state.value.copy(page = PluginTableCodec.page(bytes, description, offset))
    }
}

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
internal data class PluginTableSearchPage(val cursor: Long, val latest: Long, val truncated: Boolean,
    val rows: List<PluginTableRow>)

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
        PluginTablePage(offset, parseRows(rows, descriptor))
    }
    fun search(outcome: ByteArray, descriptor: PluginTableDescriptor, after: Long, through: Long): PluginTableSearchPage = with(PluginViewValue) {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "cursor", "latest", "truncated", "rows"))
        require(value["viewId"].id() == descriptor.viewID && value["revision"].revision() == descriptor.revision)
        val cursor = (value["cursor"] as? PluginViewValue.Value.Number)?.value ?: error("搜索游标无效")
        val latest = (value["latest"] as? PluginViewValue.Value.Number)?.value ?: error("搜索截止位置无效")
        val truncated = (value["truncated"] as? PluginViewValue.Value.Bool)?.value ?: error("搜索保留状态无效")
        val rows = value["rows"].items()
        require(latest in 0..9_007_199_254_740_991L && cursor in after..latest &&
            (through == 0L || through == latest) && (cursor > after || cursor == latest) &&
            rows.size <= 4 && (cursor > after || rows.isEmpty())) { "搜索页与请求不匹配" }
        PluginTableSearchPage(cursor, latest, truncated, parseRows(rows, descriptor))
    }
    private fun parseRows(rows: List<PluginViewValue.Value>, descriptor: PluginTableDescriptor): List<PluginTableRow> = with(PluginViewValue) {
        val parsed = rows.map {
            val row = it.fields(); row.keys(setOf("id", "cells"))
            val cells = row["cells"].items()
            require(cells.size == descriptor.columns.size) { "表格行列数不匹配" }
            PluginTableRow(row["id"].id(), captured(cells.map { cell -> cell.text(256) }))
        }
        require(parsed.map { it.id }.distinct().size == parsed.size) { "表格行 ID 重复" }
        captured(parsed)
    }
    fun utf8(value: String): ByteArray = PluginViewValue.utf8(value)
}

/** One Main owner and one read at a time, bound to a captured Runtime scope. */
internal class PluginTableModel(private val scope: CoroutineScope,
    private val request: suspend (String, ByteArray) -> ByteArray, private val expectedViewID: String? = null,
    val supportsSearch: Boolean = false,
    private val isCurrent: () -> Boolean = { true }) {
    companion object {
        val methods = listOf("view.table.describe", "view.table.page")
        const val searchMethod = "view.table.search"
    }
    data class State(val descriptor: PluginTableDescriptor? = null, val page: PluginTablePage? = null,
        val selectedOffset: Int = 0, val busy: Boolean = false, val issue: String? = null, val valid: Boolean = true,
        val searchQuery: String? = null, val searchPage: PluginTableSearchPage? = null, val searchTruncated: Boolean = false)
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
        if (!available || state.value.busy || state.value.searchQuery != null ||
            offset !in 0..lastOffset || offset % description.pageSize != 0) return null
        val expected = Any(); generation = expected
        mutable.value = state.value.copy(selectedOffset = offset, page = null, busy = true, issue = null)
        return scope.launch {
            try { readPage(description, offset, expected) }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "表格读取失败，未显示新页面：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun search(query: String): Job? {
        main()
        val description = state.value.descriptor ?: return null
        if (!supportsSearch || !available || state.value.busy || query.isEmpty() ||
            query.codePointCount(0, query.length) > 64) return null
        mutable.value = state.value.copy(searchQuery = query, searchPage = null, searchTruncated = false)
        return readSearch(description, query, 0, 0)
    }
    fun continueSearch(): Job? {
        main()
        val snapshot = state.value
        val description = snapshot.descriptor ?: return null
        val query = snapshot.searchQuery ?: return null
        val page = snapshot.searchPage ?: return null
        if (!available || snapshot.busy || page.cursor >= page.latest) return null
        return readSearch(description, query, page.cursor, page.latest)
    }
    private fun readSearch(description: PluginTableDescriptor, query: String, after: Long, through: Long): Job {
        val expected = Any(); generation = expected
        mutable.value = state.value.copy(busy = true, issue = null)
        return scope.launch {
            try {
                val input = JSONObject().put("viewId", description.viewID).put("revision", description.revision)
                    .put("query", query).put("after", after).put("through", through)
                val bytes = request(searchMethod, PluginViewValue.utf8(input.toString()))
                if (current(expected)) {
                    val page = PluginTableCodec.search(bytes, description, after, through)
                    mutable.value = state.value.copy(searchPage = page, searchTruncated = state.value.searchTruncated || page.truncated)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Exception) { if (current(expected)) mutable.value = state.value.copy(issue = "搜索失败：${failure.message}") }
            finally { if (generation === expected) mutable.value = state.value.copy(busy = false) }
        }
    }
    fun invalidate() {
        main(); generation = Any()
        mutable.value = state.value.copy(valid = false, busy = false, page = null, issue = "表格所属会话已结束",
            searchQuery = null, searchPage = null, searchTruncated = false)
    }
    private suspend fun current(expected: Any) = generation === expected && available && currentCoroutineContext().isActive
    private suspend fun readPage(description: PluginTableDescriptor, offset: Int, expected: Any) {
        val input = JSONObject().put("viewId", description.viewID).put("revision", description.revision)
            .put("offset", offset).put("limit", description.pageSize).toString().toByteArray(Charsets.UTF_8)
        val bytes = request(methods[1], input)
        if (current(expected)) mutable.value = state.value.copy(page = PluginTableCodec.page(bytes, description, offset))
    }
}

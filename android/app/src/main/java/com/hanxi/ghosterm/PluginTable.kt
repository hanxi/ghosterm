package com.hanxi.ghosterm

import android.os.Looper
import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.util.Collections
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
private fun <T> captured(values: List<T>): List<T> = Collections.unmodifiableList(ArrayList(values))

/** A bounded standard table value reader, independent of any AI backend or UI. */
internal object PluginTableCodec {
    private sealed interface Value {
        data class Fields(val values: Map<String, Value>) : Value
        data class Items(val values: List<Value>) : Value
        data class Text(val value: String) : Value
        data class Number(val value: Long) : Value
        data class Bool(val value: Boolean) : Value
        data object Null : Value
    }
    fun descriptor(outcome: ByteArray): PluginTableDescriptor {
        val value = success(outcome)
        value.keys(setOf("viewId", "revision", "title", "columns", "total", "pageSize"))
        val columns = value["columns"].items().also { require(it.size in 1..8) }.map {
            val column = it.fields(); column.keys(setOf("id", "title"))
            PluginTableColumn(column["id"].id(), column["title"].text(64, true))
        }
        require(columns.map { it.id }.distinct().size == columns.size) { "表格列 ID 重复" }
        return PluginTableDescriptor(value["viewId"].id(), value["revision"].revision(), value["title"].text(128, true),
            captured(columns), value["total"].integer(0..1000000), value["pageSize"].integer(1..16))
    }
    fun page(outcome: ByteArray, descriptor: PluginTableDescriptor, offset: Int): PluginTablePage {
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
        return PluginTablePage(offset, captured(parsed))
    }
    fun utf8(value: String): ByteArray {
        val bytes = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
        return ByteArray(bytes.remaining()).also { bytes.get(it) }
    }
    private fun success(bytes: ByteArray): Map<String, Value> {
        require(bytes.size <= 3072) { "表格回包超过上限" }
        val source = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        val root = JsonReader(StringReader(source)).use { reader ->
            reader.isLenient = false
            var nodes = 0
            fun string(): String = reader.nextString().also { utf8(it) }
            fun read(depth: Int): Value {
                require(++nodes <= 4096 && depth <= 32) { "表格 JSON 结构超过上限" }
                return when (reader.peek()) {
                    JsonToken.BEGIN_OBJECT -> {
                        reader.beginObject(); val fields = linkedMapOf<String, Value>()
                        while (reader.hasNext()) {
                            val name = reader.nextName(); utf8(name)
                            require(!fields.containsKey(name) && name !in setOf("__proto__", "constructor", "prototype")) { "表格 JSON 字段重复或无效" }
                            fields[name] = read(depth + 1)
                        }
                        reader.endObject(); Value.Fields(fields)
                    }
                    JsonToken.BEGIN_ARRAY -> {
                        reader.beginArray(); val items = mutableListOf<Value>()
                        while (reader.hasNext()) items += read(depth + 1)
                        reader.endArray(); Value.Items(items)
                    }
                    JsonToken.STRING -> Value.Text(string())
                    JsonToken.NUMBER -> {
                        val token = reader.nextString()
                        require(token.matches(Regex("-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")))
                        val number = token.toDouble()
                        require(number.isFinite() && number in -9007199254740991.0..9007199254740991.0 && number == number.toLong().toDouble()) { "表格数值必须是准确整数" }
                        Value.Number(number.toLong())
                    }
                    JsonToken.BOOLEAN -> Value.Bool(reader.nextBoolean())
                    JsonToken.NULL -> { reader.nextNull(); Value.Null }
                    else -> error("表格 JSON 格式无效")
                }
            }
            val result = read(0).fields(); require(reader.peek() == JsonToken.END_DOCUMENT) { "表格 JSON 后存在额外内容" }; result
        }
        require((root["kind"] as? Value.Text)?.value == "success") { "表格结果尚未确认" }
        root.keys(setOf("kind", "value"))
        val value = checkNotNull(root["value"])
        require(utf8(compact(value)).size <= 2048) { "表格内容超过上限" }
        return value.fields()
    }
    private fun compact(value: Value): String = when (value) {
        is Value.Fields -> value.values.entries.joinToString(",", "{", "}") { quote(it.key) + ":" + compact(it.value) }
        is Value.Items -> value.values.joinToString(",", "[", "]", transform = ::compact)
        is Value.Text -> quote(value.value)
        is Value.Number -> value.value.toString()
        is Value.Bool -> value.value.toString()
        Value.Null -> "null"
    }
    private fun quote(value: String): String = buildString {
        append('"')
        value.forEach { append(when (it) {
            '"' -> "\\\""; '\\' -> "\\\\"; '\b' -> "\\b"; '\t' -> "\\t"; '\n' -> "\\n"; '\r' -> "\\r"; '\u000c' -> "\\f"
            in '\u0000'..'\u001f' -> "\\u" + it.code.toString(16).padStart(4, '0')
            else -> it.toString()
        }) }; append('"')
    }
    private fun Value?.fields() = (this as? Value.Fields)?.values ?: error("表格字段必须是对象")
    private fun Value?.items() = (this as? Value.Items)?.values ?: error("表格字段必须是数组")
    private fun Map<String, Value>.keys(expected: Set<String>) { require(keys == expected) { "表格字段缺失或未知" } }
    private fun Value?.id(): String = (this as? Value.Text)?.value?.also {
        require(it.matches(Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,63}"))) { "表格 ID 无效" }
    } ?: error("表格 ID 必须是字符串")
    private fun Value?.revision(): String = (this as? Value.Text)?.value?.also {
        require(it.matches(Regex("[1-9][0-9]{0,19}")) && it.toULongOrNull() != null) { "表格修订无效" }
    } ?: error("表格修订必须是字符串")
    private fun Value?.integer(range: IntRange): Int {
        val value = (this as? Value.Number)?.value ?: error("表格计数必须是整数")
        require(value in range.first.toLong()..range.last.toLong()) { "表格计数超过上限" }; return value.toInt()
    }
    private fun Value?.text(maximum: Int, nonempty: Boolean = false): String = (this as? Value.Text)?.value?.also {
        require(it.codePointCount(0, it.length) <= maximum && (!nonempty || it.isNotEmpty()) &&
            it.none { c -> c.code in 0..8 || c.code in 11..12 || c.code in 14..31 || c.code == 127 }) { "表格文字无效或过长" }
    } ?: error("表格文字必须是字符串")
}

/** One Main owner and one read at a time, bound to a captured Runtime scope. */
internal class PluginTableModel(private val scope: CoroutineScope,
    private val request: suspend (String, ByteArray) -> ByteArray, private val isCurrent: () -> Boolean = { true }) {
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

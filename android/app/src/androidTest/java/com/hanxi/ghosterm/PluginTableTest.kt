package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

internal class PluginTableFixture {
    val description = JSONObject().put("viewId", "table-a").put("revision", "18446744073709551615").put("title", "Number powers")
        .put("columns", JSONArray().put(JSONObject().put("id", "n").put("title", "Number"))
            .put(JSONObject().put("id", "square").put("title", "Square")).put(JSONObject().put("id", "cube").put("title", "Cube")))
        .put("total", 1000000).put("pageSize", 16)
    val calls = mutableListOf<Pair<String, JSONObject>>()
    var pause: String? = null
    val entered = CompletableDeferred<Unit>()
    val release = CompletableDeferred<Unit>()
    var unknown = false
    var changed: ((JSONObject) -> JSONObject)? = null
    fun outcome(value: JSONObject) = JSONObject().put("kind", "success").put("value", value).toString().toByteArray(Charsets.UTF_8)
    fun page(offset: Int = 0): JSONObject {
        val total = description.getInt("total")
        return JSONObject().put("viewId", description.getString("viewId")).put("revision", description.getString("revision"))
            .put("offset", offset).put("total", total).put("rows", JSONArray().apply {
                repeat(minOf(description.getInt("pageSize"), total - offset)) { index ->
                    val n = offset.toLong() + index + 1
                    put(JSONObject().put("id", "$n").put("cells", JSONArray(listOf("$n", "${n*n}", "${n*n*n}"))))
                }
            })
    }
    suspend fun request(method: String, bytes: ByteArray): ByteArray {
        val input = JSONObject(bytes.toString(Charsets.UTF_8)); calls += method to input
        val value = when (method) {
            "view.table.describe" -> description
            PluginTableModel.searchMethod -> {
                val first = input.getLong("after") == 0L
                val rows = JSONArray()
                if (!first) rows.put(page().getJSONArray("rows").getJSONObject(0))
                JSONObject().put("viewId", description.getString("viewId"))
                    .put("revision", description.getString("revision")).put("cursor", if (first) 128 else 256)
                    .put("latest", 256).put("truncated", first).put("rows", rows)
            }
            else -> page(input.getInt("offset"))
        }
        val data = if (unknown) """{"kind":"unknown","reason":"FIXTURE_UNCONFIRMED"}""".toByteArray() else outcome(changed?.invoke(value) ?: value)
        if (pause == method) { pause = null; entered.complete(Unit); release.await() }
        return data
    }
}

@RunWith(AndroidJUnit4::class)
class PluginTableTest {
    private fun main(block: suspend CoroutineScope.() -> Unit) = runBlocking { withContext(Dispatchers.Main.immediate) { block() } }
    private fun rejected(block: () -> Unit) { try { block(); fail("Expected table value rejection") } catch (_: Exception) { } }
    private suspend fun entered(f: PluginTableFixture) { withTimeout(5000) { f.entered.await() } }

    @Test fun searchContinuesFromEmptyPageAndDiscardsLateReply() = main {
        val f = PluginTableFixture()
        val model = PluginTableModel(this, f::request, supportsSearch = true)
        model.load()!!.join()
        model.search("value")!!.join()
        assertEquals(emptyList<PluginTableRow>(), model.state.value.searchPage?.rows)
        assertEquals(128L, model.state.value.searchPage?.cursor)
        model.continueSearch()!!.join()
        assertEquals(listOf("1"), model.state.value.searchPage?.rows?.map { it.id })
        assertTrue(model.state.value.searchTruncated)
        val input = f.calls.last().second
        assertEquals(128L, input.getLong("after"))
        assertEquals(256L, input.getLong("through"))
        assertEquals("value", input.getString("query"))

        f.pause = PluginTableModel.searchMethod
        val pending = model.search("later")!!
        entered(f)
        model.invalidate()
        f.release.complete(Unit)
        pending.join()
        assertNull(model.state.value.searchPage)
        assertNull(model.state.value.searchQuery)
    }

    @Test fun revisionIsExactCanonicalPositiveUint64() {
        val f = PluginTableFixture()
        assertEquals("18446744073709551615", PluginTableCodec.descriptor(f.outcome(f.description)).revision)
        for (value in listOf("0", "01", "+1", "1\n", "1\u2028", "18446744073709551616", 1)) {
            rejected { PluginTableCodec.descriptor(f.outcome(f.description.put("revision", value))) }
        }
    }
    @Test fun descriptorRejectsUnknownFieldsBooleanAndFraction() {
        val f = PluginTableFixture()
        for ((key, value) in listOf("extra" to 1, "total" to true, "pageSize" to 1.5)) {
            val d = JSONObject(f.description.toString()).put(key, value)
            rejected { PluginTableCodec.descriptor(f.outcome(d)) }
        }
    }
    @Test fun descriptorColumnBoundsAndUniqueIdentity() {
        val f = PluginTableFixture(); val column = f.description.getJSONArray("columns").getJSONObject(0)
        for (columns in listOf(JSONArray(), JSONArray(List(9) { column }), JSONArray(listOf(column, column)))) {
            rejected { PluginTableCodec.descriptor(f.outcome(f.description.put("columns", columns))) }
        }
    }
    @Test fun textUsesUnicodeCodepointsAndRejectsControls() {
        val f = PluginTableFixture(); f.description.put("title", "😀".repeat(128))
        assertEquals(128, PluginTableCodec.descriptor(f.outcome(f.description)).title.codePointCount(0, 256))
        for (title in listOf("", "e\u0301".repeat(65), "bad\u0000text", "bad\u007ftext")) {
            rejected { PluginTableCodec.descriptor(f.outcome(f.description.put("title", title))) }
        }
    }
    @Test fun selectedPageRejectsDifferentViewRevisionOffsetAndTotal() {
        val f = PluginTableFixture(); val d = PluginTableCodec.descriptor(f.outcome(f.description))
        for ((key, value) in listOf("viewId" to "other", "revision" to "1", "offset" to 16, "total" to 2)) {
            rejected { PluginTableCodec.page(f.outcome(f.page().put(key, value)), d, 0) }
        }
    }
    @Test fun selectedPageMustContainEveryExpectedRow() {
        val f = PluginTableFixture(); val d = PluginTableCodec.descriptor(f.outcome(f.description)); val p = f.page()
        p.getJSONArray("rows").remove(15)
        rejected { PluginTableCodec.page(f.outcome(p), d, 0) }
    }
    @Test fun selectedPageRequiresColumnWidthAndUniqueRows() {
        val f = PluginTableFixture(); f.description.put("total", 2)
        val d = PluginTableCodec.descriptor(f.outcome(f.description)); val p = f.page()
        p.getJSONArray("rows").getJSONObject(0).put("cells", JSONArray(listOf("one")))
        rejected { PluginTableCodec.page(f.outcome(p), d, 0) }
        val duplicate = f.page(); duplicate.getJSONArray("rows").getJSONObject(1).put("id", "1")
        rejected { PluginTableCodec.page(f.outcome(duplicate), d, 0) }
    }
    @Test fun aggregateBodyByteLimitIsIndependentOfIndividualCells() {
        val f = PluginTableFixture(); f.description.put("total", 1)
        val d = PluginTableCodec.descriptor(f.outcome(f.description)); val p = f.page()
        p.getJSONArray("rows").getJSONObject(0).put("cells", JSONArray(List(3) { "😀".repeat(180) }))
        rejected { PluginTableCodec.page(f.outcome(p), d, 0) }
    }
    @Test fun duplicateFieldsTrailingDataSurrogatesAndMalformedUtf8AreRejected() {
        val f = PluginTableFixture(); val normal = f.outcome(f.description).toString(Charsets.UTF_8)
        for (bad in listOf(normal.replace("\"total\":1000000", "\"total\":1000000,\"total\":1000000"), normal + "{}",
            normal.replace("Number powers", "\\ud800"))) rejected { PluginTableCodec.descriptor(bad.toByteArray()) }
        rejected { PluginTableCodec.descriptor(byteArrayOf(0xc0.toByte(), 0x80.toByte())) }
    }
    @Test fun equivalentNumberSpellingsAndStringEscapingHaveTheSameValue() {
        val f = PluginTableFixture(); val normal = f.outcome(f.description).toString(Charsets.UTF_8)
        assertEquals(PluginTableCodec.descriptor(normal.toByteArray()), PluginTableCodec.descriptor(normal.replace("\"total\":1000000", "\"total\":1e6").toByteArray()))
    }
    @Test fun capturedValuesCannotBeMutatedOrChangedByInputObjects() {
        val f = PluginTableFixture(); val d = PluginTableCodec.descriptor(f.outcome(f.description)); val p = PluginTableCodec.page(f.outcome(f.page()), d, 0)
        f.description.getJSONArray("columns").getJSONObject(0).put("title", "Changed")
        assertEquals("Number", d.columns.first().title)
        rejected { (p.rows as MutableList).clear() }; rejected { (p.rows.first().cells as MutableList)[0] = "changed" }
        assertEquals("1", p.rows.first().cells.first())
    }
    @Test fun modelReadsOnlySelectedPagesAndKeepsLargeIntegersExact() = main {
        val f = PluginTableFixture(); val m = PluginTableModel(this, f::request)
        m.load()!!.join(); assertEquals(16, m.state.value.page?.rows?.size); assertFalse(m.canPrevious); assertTrue(m.canNext)
        m.showPage(m.lastOffset)!!.join()
        assertEquals(listOf("1000000", "1000000000000", "1000000000000000000"), m.state.value.page?.rows?.last()?.cells)
        assertEquals(999984, m.state.value.selectedOffset); assertFalse(m.canNext); assertTrue(m.canPrevious)
        assertEquals(3, f.calls.size); assertEquals(16, f.calls.last().second.getInt("limit"))
        assertNull(m.showPage(-1)); assertNull(m.showPage(1)); assertNull(m.showPage(1000000)); assertEquals(3, f.calls.size)
    }
    @Test fun emptyTableIsAConfirmedPage() = main {
        val f = PluginTableFixture(); f.description.put("total", 0)
        val m = PluginTableModel(this, f::request); m.load()!!.join()
        assertEquals(0, m.state.value.page?.rows?.size); assertNull(m.state.value.issue); assertFalse(m.canNext); assertFalse(m.canPrevious)
    }
    @Test fun unknownNeverBecomesEmptyOrAnAutomaticRetry() = main {
        val f = PluginTableFixture(); f.unknown = true
        val m = PluginTableModel(this, f::request); m.load()!!.join()
        assertNotNull(m.state.value.issue); assertNull(m.state.value.page); assertNull(m.state.value.descriptor); assertEquals(1, f.calls.size)
    }
    @Test fun switchingPagesClearsOldRowsAndRefusesConcurrentQueries() = main {
        val f = PluginTableFixture(); val m = PluginTableModel(this, f::request); m.load()!!.join()
        f.pause = "view.table.page"; val job = m.showPage(16)!!; entered(f)
        assertTrue(m.state.value.busy); assertNull(m.state.value.page); assertEquals(16, m.state.value.selectedOffset)
        assertNull(m.showPage(32)); assertEquals(3, f.calls.size)
        f.release.complete(Unit); job.join(); assertEquals("17", m.state.value.page?.rows?.first()?.id)
    }
    @Test fun lateDescriptorCannotStartReadsForAReplacedRuntime() = main {
        val f = PluginTableFixture(); var current = true
        val m = PluginTableModel(this, f::request) { current }; f.pause = "view.table.describe"
        val job = m.load()!!; entered(f); current = false; f.release.complete(Unit); job.join()
        assertNull(m.state.value.descriptor); assertNull(m.state.value.page); assertEquals(1, f.calls.size)
    }
    @Test fun latePageCannotReopenAnInvalidatedView() = main {
        val f = PluginTableFixture(); val m = PluginTableModel(this, f::request); m.load()!!.join()
        f.pause = "view.table.page"; val job = m.showPage(16)!!; entered(f)
        m.invalidate(); f.release.complete(Unit); job.join()
        assertTrue(m.state.value.page == null && !m.state.value.valid && !m.state.value.busy && m.state.value.issue == "表格所属会话已结束")
        assertNull(m.load()); assertEquals(3, f.calls.size)
    }
    @Test fun nativeBootstrapDeclaresOnlyItsImplementedClientService() {
        val hash = "a".repeat(64)
        val request = ExtensionRelayBootstrap.Request("/gterm", "/package", hash, hash, PluginTableModel.methods,
            command = "table", clientCapabilities = listOf("ui.table"))
        assertTrue(ExtensionRelayBootstrap.build(request).contains("'--client-capability' 'ui.table'"))
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.unknown"))) }
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.table", "ui.table"))) }
    }
}

package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

internal class PluginDiffFixture {
    val description = JSONObject().put("viewId", "diff-a").put("revision", "18446744073709551615").put("title", "Text comparison")
        .put("beforeLabel", "Before").put("afterLabel", "After").put("beforeLines", 3).put("afterLines", 3)
        .put("added", 2).put("removed", 2).put("total", 5).put("pageSize", 3)
    fun row(kind: String, before: Int, after: Int, text: String, ending: String) = JSONObject()
        .put("kind", kind).put("beforeLine", before).put("afterLine", after).put("text", text).put("ending", ending)
    var rows = listOf(row("equal", 1, 1, "alpha", "crlf"), row("delete", 2, 0, "beta", "lf"),
        row("delete", 3, 0, "last", "none"), row("insert", 0, 2, "gamma", "lf"), row("insert", 0, 3, "last", "lf"))
    val calls = mutableListOf<Pair<String, JSONObject>>()
    var pause: String? = null
    val entered = CompletableDeferred<Unit>()
    val release = CompletableDeferred<Unit>()
    var unknown = false
    fun outcome(value: JSONObject) = JSONObject().put("kind", "success").put("value", value).toString().toByteArray(Charsets.UTF_8)
    fun page(offset: Int = 0): JSONObject = JSONObject().put("viewId", description.getString("viewId"))
        .put("revision", description.getString("revision")).put("offset", offset).put("total", description.getInt("total"))
        .put("beforeOffset", rows.take(offset).count { it.getString("kind") != "insert" })
        .put("afterOffset", rows.take(offset).count { it.getString("kind") != "delete" })
        .put("rows", JSONArray(rows.drop(offset).take(description.getInt("pageSize")).map { JSONObject(it.toString()) }))
    suspend fun request(method: String, bytes: ByteArray): ByteArray {
        val input = JSONObject(bytes.toString(Charsets.UTF_8)); calls += method to input
        val data = if (unknown) """{"kind":"unknown","reason":"FIXTURE_UNCONFIRMED"}""".toByteArray()
            else outcome(if (method == "view.diff.describe") description else page(input.getInt("offset")))
        if (pause == method) { pause = null; entered.complete(Unit); release.await() }
        return data
    }
    fun empty() { rows = emptyList(); for (key in listOf("beforeLines", "afterLines", "added", "removed", "total")) description.put(key, 0) }
}

@RunWith(AndroidJUnit4::class)
class PluginDiffTest {
    private fun main(block: suspend CoroutineScope.() -> Unit) = runBlocking { withContext(Dispatchers.Main.immediate) { block() } }
    private fun rejected(block: () -> Unit) { try { block(); fail("Expected diff value rejection") } catch (_: Exception) { } }
    private fun decode(f: PluginDiffFixture) = PluginDiffCodec.descriptor(f.outcome(f.description))
    private fun page(f: PluginDiffFixture, value: JSONObject = f.page(), offset: Int = 0, prefix: PluginDiffCursor = PluginDiffCursor.ZERO) = PluginDiffCodec.page(f.outcome(value), decode(f), offset, prefix)
    private suspend fun entered(f: PluginDiffFixture) { withTimeout(5000) { f.entered.await() } }

    @Test fun revisionIsExactCanonicalPositiveUint64() {
        val f = PluginDiffFixture(); assertEquals("18446744073709551615", decode(f).revision)
        for (value in listOf("0", "01", "+1", "1\n", "1\u2028", "18446744073709551616", 1)) {
            rejected { decode(f.apply { description.put("revision", value) }) }
        }
    }
    @Test fun descriptorRejectsCountMismatchBoundsUnknownBooleanAndFraction() {
        val f = PluginDiffFixture()
        for ((key, value) in listOf("beforeLines" to 4, "afterLines" to 4, "added" to 4, "removed" to -1,
            "total" to 4, "pageSize" to 4, "total" to true, "added" to 1.5, "extra" to 0)) {
            rejected { PluginDiffCodec.descriptor(f.outcome(JSONObject(f.description.toString()).put(key, value))) }
        }
    }
    @Test fun labelsUseUnicodeCodepointsAndRejectControls() {
        val f = PluginDiffFixture(); f.description.put("title", "😀".repeat(128)); assertEquals(128, decode(f).title.codePointCount(0, 256))
        for ((key, value) in listOf("viewId" to "x\n", "beforeLabel" to "", "afterLabel" to "bad\u0000", "title" to "e\u0301".repeat(65))) {
            rejected { PluginDiffCodec.descriptor(f.outcome(JSONObject(f.description.toString()).put(key, value))) }
        }
    }
    @Test fun selectedPageRejectsDifferentViewRevisionOffsetAndTotal() {
        val f = PluginDiffFixture()
        for ((key, value) in listOf("viewId" to "other", "revision" to "1", "offset" to 3, "total" to 6, "extra" to 0)) {
            rejected { page(f, f.page().put(key, value)) }
        }
    }
    @Test fun selectedPageMustContainEveryExpectedRow() {
        val f = PluginDiffFixture(); val p = f.page(); p.getJSONArray("rows").remove(2)
        rejected { page(f, p) }
    }
    @Test fun selectedPageRequiresExactBeforeAndAfterPrefix() {
        val f = PluginDiffFixture(); val prefix = PluginDiffCursor(3, 1)
        rejected { page(f, f.page(3).put("beforeOffset", 2), 3, prefix) }
        rejected { page(f, f.page(3).put("afterOffset", 0), 3, prefix) }
    }
    @Test fun rowCoordinatesKindsEndingsAndTextAreExact() {
        val f = PluginDiffFixture()
        for ((key, value) in listOf("kind" to "update", "beforeLine" to 0, "afterLine" to 2, "ending" to "unknown", "text" to "a\r\nb", "text" to "bad\u0000")) {
            val p = f.page(); p.getJSONArray("rows").getJSONObject(0).put(key, value)
            rejected { page(f, p) }
        }
    }
    @Test fun noFinalNewlineIsOnlyValidOnFinalSourceLine() {
        val f = PluginDiffFixture(); val p = f.page(); p.getJSONArray("rows").getJSONObject(0).put("ending", "none")
        rejected { page(f, p) }
        assertEquals(PluginDiffRow.Ending.NONE, page(f).rows.last().ending)
    }
    @Test fun finalPageMustConsumeBothSources() {
        val f = PluginDiffFixture(); f.description.put("beforeLines", 4).put("afterLines", 4).put("total", 6)
        val p = f.page(3).put("rows", JSONArray(listOf(f.row("insert", 0, 2, "two", "lf"),
            f.row("insert", 0, 3, "three", "lf"), f.row("delete", 4, 0, "four", "lf"))))
        rejected { page(f, p, 3, PluginDiffCursor(3, 1)) }
    }
    @Test fun wideUnicodeAndStrictRawJsonAreSupported() {
        val f = PluginDiffFixture(); f.description.put("added", 0).put("removed", 0).put("total", 3)
        f.rows = (1..3).map { f.row("equal", it, it, "😀".repeat(128), "lf") }; assertEquals(3, page(f).rows.size)
        val normal = f.outcome(f.description).toString(Charsets.UTF_8)
        for (bad in listOf(normal + "{}", normal.replace("\"total\":3", "\"total\":3,\"total\":3"), normal.replace("Text comparison", "\\ud800"))) {
            rejected { PluginDiffCodec.descriptor(bad.toByteArray()) }
        }
        rejected { PluginDiffCodec.descriptor(byteArrayOf(0xc0.toByte(), 0x80.toByte())) }
    }
    @Test fun capturedRowsAreImmutableAndIndependentOfInput() {
        val f = PluginDiffFixture(); val p = page(f); f.rows[0].put("text", "changed")
        assertEquals("alpha", p.rows[0].text); rejected { (p.rows as MutableList).clear() }
    }
    @Test fun modelUsesOnlyReachableVerifiedCursorsAndRequeriesPreviousPage() = main {
        val f = PluginDiffFixture(); val m = PluginDiffModel(this, f::request); m.load()!!.join()
        assertNull(m.state.value.issue); assertEquals(listOf("alpha", "beta", "last"), m.state.value.page?.rows?.map { it.text })
        assertTrue(m.canNext); assertFalse(m.canPrevious)
        m.showPage(3)!!.join(); assertEquals(PluginDiffCursor(3, 1), m.state.value.page?.start)
        assertEquals(listOf("gamma", "last"), m.state.value.page?.rows?.map { it.text }); assertFalse(m.canNext); assertTrue(m.canPrevious)
        m.showPage(0)!!.join(); assertEquals(PluginDiffRow.Ending.NONE, m.state.value.page?.rows?.last()?.ending)
        val count = f.calls.size; assertNull(m.showPage(1)); assertNull(m.showPage(5)); assertNull(m.showPage(-1)); assertEquals(count, f.calls.size)
    }
    @Test fun unvisitedPageIsRejectedWithoutRpc() = main {
        val f = PluginDiffFixture(); f.description.put("beforeLines", 9).put("afterLines", 9).put("added", 0).put("removed", 0).put("total", 9)
        f.rows = (1..9).map { f.row("equal", it, it, "line", "lf") }
        val m = PluginDiffModel(this, f::request); m.load()!!.join(); assertNull(m.showPage(6)); assertEquals(2, f.calls.size)
    }
    @Test fun emptyIsAConfirmedPage() = main {
        val f = PluginDiffFixture(); f.empty(); val m = PluginDiffModel(this, f::request); m.load()!!.join()
        assertEquals(0, m.state.value.page?.rows?.size); assertNull(m.state.value.issue); assertFalse(m.canNext); assertFalse(m.canPrevious)
    }
    @Test fun unknownNeverBecomesEmptyOrAnAutomaticRetry() = main {
        val f = PluginDiffFixture(); f.unknown = true; val m = PluginDiffModel(this, f::request); m.load()!!.join()
        assertNotNull(m.state.value.issue); assertNull(m.state.value.page); assertNull(m.state.value.descriptor); assertEquals(1, f.calls.size)
    }
    @Test fun switchingPagesClearsOldRowsAndRefusesConcurrentQueries() = main {
        val f = PluginDiffFixture(); val m = PluginDiffModel(this, f::request); m.load()!!.join()
        f.pause = "view.diff.page"; val job = m.showPage(3)!!; entered(f)
        assertTrue(m.state.value.busy); assertNull(m.state.value.page); assertEquals(3, m.state.value.selectedOffset)
        assertNull(m.showPage(0)); assertEquals(3, f.calls.size)
        f.release.complete(Unit); job.join(); assertEquals(3, m.state.value.page?.offset)
    }
    @Test fun lateDescriptorCannotStartReadsForAReplacedRuntime() = main {
        val f = PluginDiffFixture(); var current = true; val m = PluginDiffModel(this, f::request) { current }; f.pause = "view.diff.describe"
        val job = m.load()!!; entered(f); current = false; f.release.complete(Unit); job.join()
        assertNull(m.state.value.descriptor); assertNull(m.state.value.page); assertEquals(1, f.calls.size)
    }
    @Test fun latePageCannotReopenAnInvalidatedView() = main {
        val f = PluginDiffFixture(); val m = PluginDiffModel(this, f::request); m.load()!!.join()
        f.pause = "view.diff.page"; val job = m.showPage(3)!!; entered(f)
        m.invalidate(); f.release.complete(Unit); job.join()
        assertTrue(m.state.value.page == null && !m.state.value.valid && !m.state.value.busy && m.state.value.issue == "差异所属会话已结束")
        assertNull(m.load()); assertEquals(3, f.calls.size)
    }
    @Test fun nativeBootstrapDeclaresOnlyUniqueImplementedClientServices() {
        val hash = "a".repeat(64); val request = ExtensionRelayBootstrap.Request("/gterm", "/package", hash, hash,
            PluginDiffModel.methods, command = "diff", clientCapabilities = listOf("ui.diff"))
        assertTrue(ExtensionRelayBootstrap.build(request).contains("'--client-capability' 'ui.diff'"))
        val both = ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.table", "ui.diff")))
        assertTrue(both.contains("'--client-capability' 'ui.table'") && both.contains("'--client-capability' 'ui.diff'"))
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.unknown"))) }
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.diff", "ui.diff"))) }
    }
}

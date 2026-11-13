package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CodexChangeJsonTest {
    private fun source(changes: JSONArray = JSONArray()): JSONObject = JSONObject()
        .put("schemaVersion", 1).put("threadId", "thread").put("turnId", "turn").put("itemId", "item")
        .put("changes", changes)

    private fun file(path: String = "src/中文😀.kt", diff: String = "+中文😀e\u0301\n"): JSONObject = JSONObject()
        .put("path", path).put("kind", JSONObject().put("type", "update").put("move_path", JSONObject.NULL))
        .put("diff", diff)

    private fun reference(text: String): CodexChangeReference = checkNotNull(CodexChangeReference.create(
        "change-1", "thread", "turn", "item", text.length.toLong(), codexChangeUtf8(text).size.toLong()))

    private fun decode(text: String) = CodexChangeJson.document(codexChangeUtf8(text), reference(text))

    @Test fun completeUnicodeAndTenFilesSurviveBeyondThePreviewLimit() {
        val diff = (1..600).joinToString("\n") { "+$it 中文😀 e\u0301" } + "\n+last-line\n"
        val changes = JSONArray()
        repeat(10) { changes.put(file("src/文件-$it.kt", diff)) }
        val document = decode(source(changes).toString())
        assertEquals(10, document.files.size)
        assertEquals("src/文件-9.kt", document.files.last().path)
        assertEquals(diff, document.files.last().diff)
        assertNull(document.files.last().movePath)
    }

    @Test fun metadataAndChunkCountsRequireIntegerTypes() {
        val marker = JSONObject().put("snapshotID", "change-1").put("snapshotUnits", 5).put("snapshotBytes", 5)
        assertNotNull(CodexChangeJson.reference(marker, "thread", "turn", "item"))
        for (bad in listOf(true, 5.0, "5", -1, 524289L)) {
            assertNull("metadata $bad", CodexChangeJson.reference(JSONObject(marker.toString()).put("snapshotBytes", bad),
                "thread", "turn", "item"))
        }
        val chunk = JSONObject().put("status", "ok").put("snapshotID", "change-1").put("offset", 0)
            .put("nextOffset", 5).put("totalUnits", 5).put("totalBytes", 5).put("content", "abcde").put("done", true)
        assertTrue(CodexChangeJson.chunk(chunk).done)
        for (bad in listOf(true, 1.0, "1", -1, 524289L)) {
            assertThrows(Exception::class.java) { CodexChangeJson.chunk(JSONObject(chunk.toString()).put("offset", bad)) }
        }
        assertThrows(Exception::class.java) { CodexChangeJson.chunk(JSONObject(chunk.toString()).put("done", "true")) }
        assertThrows(Exception::class.java) { CodexChangeJson.chunk(JSONObject(chunk.toString()).put("content", 12)) }
    }

    @Test fun schemaRejectsDuplicatesUnknownFieldsAndCoercedVersionOrDiff() {
        val valid = source(JSONArray().put(file())).toString()
        val duplicate = valid.replace("\"schemaVersion\":1", "\"schemaVersion\":1,\"schemaVersion\":1")
        val invalid = listOf(duplicate, valid.replace("\"schemaVersion\":1", "\"schemaVersion\":1.0"),
            valid.replace("\"schemaVersion\":1", "\"schemaVersion\":\"1\""),
            JSONObject(valid).put("unknown", JSONObject()).toString(),
            source(JSONArray().put(file().put("diff", 123))).toString(),
            source(JSONArray().put(file().put("extra", 1))).toString(),
            valid + " {}")
        for (value in invalid) assertThrows("invalid document: $value", Exception::class.java) { decode(value) }
    }

    @Test fun documentIdentityIsExactAndMalformedEncodingIsRejected() {
        val value = source().put("threadId", "thréad").toString()
        val ref = reference(value).copy(threadID = "thre\u0301ad")
        assertThrows(Exception::class.java) { CodexChangeJson.document(codexChangeUtf8(value), ref) }
        val valid = source().toString()
        val bytes = codexChangeUtf8(valid)
        bytes[0] = 0xff.toByte()
        assertThrows(Exception::class.java) { CodexChangeJson.document(bytes, reference(valid)) }
        assertThrows(Exception::class.java) { decode(valid.replace("\"thread\"", "\"\\ud800\"")) }
    }

    @Test fun changeKindsPathsAndFileCountHaveExplicitLimits() {
        val moved = file().put("kind", JSONObject().put("type", "update").put("move_path", "new/目标.kt"))
        assertEquals("new/目标.kt", decode(source(JSONArray().put(moved)).toString()).files.single().movePath)
        val invalid = listOf(file().put("kind", JSONObject().put("type", "update")),
            file().put("kind", JSONObject().put("type", "add").put("move_path", JSONObject.NULL)),
            file().put("kind", JSONObject().put("type", "other")), file(path = ""), file(path = "x".repeat(16385)))
        for (value in invalid) assertThrows(Exception::class.java) { decode(source(JSONArray().put(value)).toString()) }
        val many = JSONArray()
        repeat(8193) { many.put(JSONObject().put("path", "a").put("kind", JSONObject().put("type", "add")).put("diff", "")) }
        assertThrows(Exception::class.java) { decode(source(many).toString()) }
    }

    @Test fun actualChunkJsonReassemblesOnlyAfterExactEndOfDocument() {
        val text = source(JSONArray().put(file())).toString()
        val ref = reference(text)
        val assembler = CodexChangeAssembler(ref)
        val first = text.substring(0, 5)
        fun chunk(offset: Int, content: String, done: Boolean) = CodexChangeJson.chunk(JSONObject()
            .put("status", "ok").put("snapshotID", ref.snapshotID).put("offset", offset)
            .put("nextOffset", offset + content.length).put("totalUnits", ref.totalUnits)
            .put("totalBytes", ref.totalBytes).put("content", content).put("done", done))
        assembler.append(chunk(0, first, false))
        assertThrows(Exception::class.java) { assembler.finishBytes() }
        assembler.append(chunk(5, text.substring(5), true))
        assertEquals("src/中文😀.kt", CodexChangeJson.document(assembler.finishBytes(), ref).files.single().path)
    }
}

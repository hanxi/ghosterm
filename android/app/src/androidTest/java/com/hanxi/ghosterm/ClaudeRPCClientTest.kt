package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Named in-memory document RPC fixture, not an SDK, CLI or model. */
private class ClaudeDocumentFixture {
    val calls = mutableListOf<String>()
    var uploaded = ""
    var reply = byteArrayOf()
    var sequence = 0
    var page: (JSONObject) -> JSONObject = { it }
    var record: (ByteArray) -> ByteArray = { it }
    var opened: (JSONObject) -> JSONObject = { it }
    var append: (JSONObject) -> JSONObject = { it }
    var envelope: (JSONObject) -> JSONObject = { it }
    var entered: CompletableDeferred<Unit>? = null
    var release: CompletableDeferred<Unit>? = null
    suspend fun rpc(method: String, bytes: ByteArray): ByteArray {
        check(bytes.size <= 3072)
        val input = ClaudeJson.objectValue(bytes)
        calls += method
        if (method == "claude.call") { entered?.complete(Unit); release?.await() }
        val value = when (method) {
            "claude.input.open" -> { uploaded = ""; opened(JSONObject().put("status", "ok").put("inputID", "input-1").put("nextOffset", 0)) }
            "claude.input.append" -> {
                check(input.getInt("offset") == uploaded.length)
                val content = input.getString("content"); ClaudeJson.utf8(content)
                uploaded += content
                append(JSONObject().put("status", "ok").put("nextOffset", uploaded.length))
            }
            "claude.input.seal" -> JSONObject().put("status", "ok")
            "claude.call" -> {
                sequence++
                val child = JSONObject().put("version", 1).put("id", "command-$sequence").put("sequence", sequence)
                    .put("ok", true).put("result", JSONObject().put("text", "中文😀/\n".repeat(700)).put("uploaded", uploaded))
                reply = record(ClaudeJson.utf8(child.toString()))
                JSONObject().put("status", "ok").put("responseID", "response-1")
                    .put("totalUnits", reply.toString(Charsets.UTF_8).length).put("totalBytes", reply.size)
            }
            "claude.response.read" -> {
                val text = reply.toString(Charsets.UTF_8); val offset = input.getInt("offset")
                var end = minOf(text.length, offset + 256)
                if (end < text.length && text[end - 1].isHighSurrogate()) end--
                page(JSONObject().put("status", "ok").put("responseID", "response-1").put("offset", offset)
                    .put("nextOffset", end).put("totalUnits", text.length).put("totalBytes", reply.size)
                    .put("content", text.substring(offset, end)).put("done", end == text.length))
            }
            "claude.response.forget" -> JSONObject().put("accepted", true)
            else -> error("Unexpected fixture method: $method")
        }
        return ClaudeJson.utf8(envelope(JSONObject().put("kind", "success").put("value", value)).toString())
    }
}

@RunWith(AndroidJUnit4::class)
class ClaudeRPCClientTest {
    private fun main(block: suspend () -> Unit) = runBlocking { withContext(Dispatchers.Main) { block() } }
    private suspend fun rejected(message: String, block: suspend () -> Unit) {
        var failed = false
        try { block() } catch (_: Exception) { failed = true }
        assertTrue(message, failed)
    }

    @Test fun completeUnicodeParametersAndReplyKeepExactBytes() = main {
        val fixture = ClaudeDocumentFixture(); val client = ClaudeRPCClient(fixture::rpc)
        val input = "{\"answers\":{\"é\":\"one\",\"e\u0301\":\"two\"},\"text\":\"" + "中文😀/".repeat(900) + "\"}"
        val result = client.request("answer", ClaudeJson.utf8(input))
        assertEquals("Full parameter text must survive upload", input, fixture.uploaded)
        assertArrayEquals("All response bytes must survive pagination", fixture.reply, result)
        assertEquals(1, fixture.calls.count { it == "claude.call" })
        assertEquals("claude.response.forget", fixture.calls.last())
        client.request("poll", "{}".toByteArray()); assertEquals(2, fixture.sequence)
    }

    @Test fun pageIdentityOffsetsCountsAndCompletionMustMatch() = main {
        for (field in listOf("responseID", "offset", "nextOffset", "totalUnits", "totalBytes", "content", "done")) {
            val fixture = ClaudeDocumentFixture()
            fixture.page = { value -> value.put(field, when (field) {
                "responseID" -> "response-999"
                "content" -> ""
                "done" -> !value.getBoolean(field)
                else -> value.getInt(field) + 1
            }) }
            val client = ClaudeRPCClient(fixture::rpc)
            rejected("Claude must reject mismatched page $field") { client.request("poll", "{}".toByteArray()) }
            assertFalse("Invalid receipts must not be discarded", "claude.response.forget" in fixture.calls)
        }
    }

    @Test fun childIdentityDuplicateFieldsAndUnknownShapesMustBeRejected() = main {
        for (mode in listOf("id", "sequence", "duplicate", "result", "boolean", "extra")) {
            val fixture = ClaudeDocumentFixture()
            fixture.record = { bytes ->
                var text = bytes.toString(Charsets.UTF_8)
                text = when (mode) {
                    "id" -> text.replace("command-1", "command-9")
                    "sequence" -> text.replace("\"sequence\":1", "\"sequence\":9")
                    "duplicate" -> text.replace("\"ok\":true", "\"ok\":true,\"\\u006fk\":true")
                    "boolean" -> text.replace("\"ok\":true", "\"ok\":1")
                    "extra" -> text.dropLast(1) + ",\"extra\":true}"
                    else -> "{\"version\":1,\"id\":\"command-1\",\"sequence\":1,\"ok\":true}"
                }
                text.toByteArray(Charsets.UTF_8)
            }
            rejected("Claude must reject child identity/schema $mode") { ClaudeRPCClient(fixture::rpc).request("poll", "{}".toByteArray()) }
            assertFalse("Invalid child envelopes must remain observable", "claude.response.forget" in fixture.calls)
        }
    }

    @Test fun invalidationRejectsLateRepliesAndBusyCannotSendAnotherAction() = main {
        coroutineScope {
            val fixture = ClaudeDocumentFixture()
            fixture.entered = CompletableDeferred(); fixture.release = CompletableDeferred()
            val client = ClaudeRPCClient(fixture::rpc)
            val first = async { runCatching { client.request("send", "{}".toByteArray()) } }
            fixture.entered!!.await()
            rejected("Busy client cannot dispatch another action") { client.request("send", "{}".toByteArray()) }
            client.invalidate(); fixture.release!!.complete(Unit)
            assertTrue("A late reply cannot continue reading after invalidation", first.await().isFailure)
            assertFalse("No read after invalidation", "claude.response.read" in fixture.calls)
            val before = fixture.calls.size
            rejected("Invalidated client cannot replay") { client.request("poll", "{}".toByteArray()) }
            assertEquals(before, fixture.calls.size)
        }
    }

    @Test fun uncertainOperationClosesScopeAndNeverRetries() = main {
        val fixture = ClaudeDocumentFixture()
        val client = ClaudeRPCClient { method, bytes ->
            if (method == "claude.call") { fixture.calls += method; error("explicit uncertain fixture") }
            fixture.rpc(method, bytes)
        }
        rejected("Uncertain operation must fail") { client.request("send", "{}".toByteArray()) }
        val before = fixture.calls.size
        rejected("Uncertain scope must refuse another effect") { client.request("send", "{}".toByteArray()) }
        assertEquals("No implicit replay after uncertainty", before, fixture.calls.size)
        assertEquals("input-1", client.lastInputID); assertNull(client.lastResponseID)
    }

    @Test fun invalidParametersCannotStartUploadAndValidCallStillWorks() = main {
        val fixture = ClaudeDocumentFixture(); val client = ClaudeRPCClient(fixture::rpc)
        for (input in listOf("[]", "{\"x\":1,\"x\":2}", "{} trailing", "{\"x\":\"\\uD800\"}", "{\"x\":01}", "{\"x\":NaN}", "{\"x\":\"" + "x".repeat(64000) + "\"}")) {
            rejected("Invalid parameters must be rejected") { client.request("send", input.toByteArray()) }
        }
        rejected("Malformed UTF8 must not be replaced") { client.request("send", byteArrayOf(123, 34, -1, 34, 58, 48, 125)) }
        assertTrue("Validation cannot perform RPC", fixture.calls.isEmpty())
        client.request("poll", "{}".toByteArray())
    }

    @Test fun noncanonicalIDsAndRefusedUploadMustStopBeforeChildAction() = main {
        for (id in listOf("input-0", "input-01", "input-+1", "input-9007199254740992")) {
            val fixture = ClaudeDocumentFixture(); fixture.opened = { it.put("inputID", id) }
            rejected("Input IDs must be canonical") { ClaudeRPCClient(fixture::rpc).request("poll", "{}".toByteArray()) }
            assertFalse("Bad input ID cannot authorize an action", "claude.call" in fixture.calls)
        }
        val fixture = ClaudeDocumentFixture(); fixture.append = { it.put("status", "refused") }
        rejected("Refused upload cannot authorize an action") { ClaudeRPCClient(fixture::rpc).request("send", "{}".toByteArray()) }
        assertFalse("claude.call" in fixture.calls)
    }

    @Test fun strictJsonPreservesUnicodeKeysAndRejectsStructureBudgets() = main {
        val value = ClaudeJson.objectValue("{\"é\":1,\"e\u0301\":2}".toByteArray())
        assertEquals(2, value.length())
        for (text in listOf("{\"x\":" + "[".repeat(41) + "0" + "]".repeat(41) + "}",
                "{\"x\":[" + List(10000) { "0" }.joinToString(",") + "]}")) {
            rejected("JSON structure limits must be enforced") { ClaudeJson.objectValue(text.toByteArray()) }
        }
    }
}

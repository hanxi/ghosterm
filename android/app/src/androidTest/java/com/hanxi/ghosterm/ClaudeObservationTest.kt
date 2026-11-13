package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Named document endpoint, not an SDK/CLI or SSH execution. */
internal class ClaudeObservationFixture {
    val calls = mutableListOf<String>()
    var failurePoint = "call"
    var inputID = "input-1"; var responseID = "response-2"
    var statusInput: String? = null; var statusResponse: String? = null
    var emptyStatus = false; var exitJSON = "null"
    var sequence = 0; var body = byteArrayOf()
    var page: (JSONObject) -> JSONObject = { it }
    var record: (ByteArray) -> ByteArray = { it }
    var pause: String? = null
    var entered = CompletableDeferred<Unit>(); var release = CompletableDeferred<Unit>()
    suspend fun rpc(method: String, bytes: ByteArray): ByteArray {
        calls += method; val input = ClaudeJson.objectValue(bytes, 3072)
        if (method == pause) { pause = null; entered.complete(Unit); release.await() }
        val value = when (method) {
            "claude.input.open" -> {
                check(failurePoint != "open") { "fixture open loss" }
                JSONObject().put("status", "ok").put("inputID", inputID).put("nextOffset", 0)
            }
            "claude.input.append" -> JSONObject().put("status", "ok").put("nextOffset", input.getInt("offset") + input.getString("content").length)
            "claude.input.seal" -> JSONObject().put("status", "ok")
            "claude.call" -> {
                sequence++
                val child = JSONObject().put("version", 1).put("id", "command-$sequence").put("sequence", sequence).put("ok", true)
                    .put("result", JSONObject().put("é", "one").put("e\u0301", "two").put("text", "中文😀/\n".repeat(800)))
                body = record(ClaudeJson.utf8(child.toString()))
                check(failurePoint != "call") { "fixture call receipt lost" }
                JSONObject().put("status", "ok").put("responseID", responseID).put("totalUnits", body.toString(Charsets.UTF_8).length).put("totalBytes", body.size)
            }
            "claude.status" -> JSONObject().put("phase", "live").put("failure", "").put("exitJSON", exitJSON).put("reaped", false)
                .put("inputID", if (emptyStatus) "" else statusInput ?: inputID).put("responseID", if (emptyStatus) "" else statusResponse ?: responseID)
            "claude.response.read" -> {
                val text = body.toString(Charsets.UTF_8); val offset = input.getInt("offset")
                var end = minOf(text.length, offset + 256)
                if (end < text.length && text[end - 1].isHighSurrogate()) end--
                page(JSONObject().put("status", "ok").put("responseID", responseID).put("offset", offset).put("nextOffset", end)
                    .put("totalUnits", text.length).put("totalBytes", body.size).put("content", text.substring(offset, end)).put("done", end == text.length))
            }
            "claude.response.forget" -> { check(failurePoint != "forget") { "fixture forget receipt lost" }; JSONObject().put("accepted", true) }
            else -> error("Unexpected fixture method $method")
        }
        return ClaudeJson.utf8(JSONObject().put("kind", "success").put("value", value).toString())
    }
}

@RunWith(AndroidJUnit4::class)
class ClaudeObservationTest {
    private fun main(block: suspend () -> Unit) { runBlocking { withContext(Dispatchers.Main) { block() } } }
    private suspend fun lost(fixture: ClaudeObservationFixture): ClaudeRPCClient {
        val client = ClaudeRPCClient(fixture::rpc)
        try { client.request("send", "{}".toByteArray()); fail("Expected fixture loss") } catch (_: Exception) { }
        return client
    }
    @Test fun lostCallReadsExactOriginalWithoutEffectsOrReplay() = main {
        val fixture = ClaudeObservationFixture(); val client = lost(fixture); val before = fixture.calls.size
        val result = client.observe()
        assertEquals(ClaudeRPCClient.Observation.Source.EXECUTOR, result.source)
        assertArrayEquals(fixture.body, result.response)
        assertEquals("input-1", result.inputID); assertEquals("response-2", result.responseID)
        assertTrue(result.response!!.size > 3072)
        assertTrue("Observation must be read-only", fixture.calls.drop(before).all { it in listOf("claude.status", "claude.response.read") })
        val count = fixture.calls.size; assertArrayEquals(fixture.body, client.observe().response); assertEquals(count, fixture.calls.size)
        var rejected = false
        try { client.request("send", "{}".toByteArray()) } catch (_: Exception) { rejected = true }
        assertTrue("Observation cannot reopen effects", rejected && fixture.calls.count { it == "claude.call" } == 1)
    }
    @Test fun lostForgetUsesValidatedBytesWithoutAnotherRPC() = main {
        val fixture = ClaudeObservationFixture().apply { failurePoint = "forget" }; val client = lost(fixture)
        val before = fixture.calls.size; val result = client.observe()
        assertTrue("Unknown forget cannot trigger another release/action", result.source == ClaudeRPCClient.Observation.Source.RECEIVED &&
            result.response!!.contentEquals(fixture.body) && before == fixture.calls.size)
    }
    @Test fun absentReceiptNeverMeansNotExecuted() = main {
        val fixture = ClaudeObservationFixture().apply { emptyStatus = true }; val result = lost(fixture).observe()
        assertEquals(ClaudeRPCClient.Observation.Source.UNAVAILABLE, result.source); assertNull(result.response)
        assertFalse(fixture.calls.contains("claude.response.forget"))
    }
    @Test fun foreignInputOrResponseCannotBeAdopted() = main {
        for (mode in listOf("input", "response", "known")) {
            val fixture = ClaudeObservationFixture()
            if (mode == "input") fixture.statusInput = "input-999"
            else if (mode == "response") fixture.statusResponse = "response-02"
            else { fixture.failurePoint = ""; fixture.statusResponse = "response-999"; fixture.page = { it.put("done", true) } }
            val client = lost(fixture); val before = fixture.calls.size; var rejected = false
            try { client.observe() } catch (_: Exception) { rejected = true }
            assertTrue("Foreign observation cannot read/adopt a response: $mode", rejected && fixture.calls.drop(before) == listOf("claude.status"))
        }
    }
    @Test fun failedReadCannotChangeTheFirstResponseIdentity() = main {
        val fixture = ClaudeObservationFixture(); val client = lost(fixture)
        fixture.page = { it.put("done", true) }
        try { client.observe(); fail("Expected invalid first page") } catch (_: Exception) { }
        fixture.page = { it }; fixture.responseID = "response-999"
        val before = fixture.calls.size; var rejected = false
        try { client.observe() } catch (_: Exception) { rejected = true }
        assertTrue("A failed read cannot authorize a different response", rejected && fixture.calls.drop(before) == listOf("claude.status"))
    }
    @Test fun observedPagesAndChildIdentityKeepAllChecks() = main {
        for (field in listOf("responseID", "offset", "nextOffset", "totalUnits", "totalBytes", "done", "content", "id", "sequence", "duplicate")) {
            val fixture = ClaudeObservationFixture()
            if (field in listOf("id", "sequence", "duplicate")) fixture.record = { data ->
                val old = when (field) { "id" -> "command-1"; "sequence" -> "\"sequence\":1"; else -> "\"ok\":true" }
                val new = when (field) { "id" -> "command-9"; "sequence" -> "\"sequence\":9"; else -> "\"ok\":true,\"ok\":true" }
                ClaudeJson.utf8(data.toString(Charsets.UTF_8).replace(old, new))
            } else fixture.page = { value -> value.put(field, when (field) {
                "responseID" -> "response-999"; "done" -> !value.getBoolean(field); "content" -> ""; else -> value.getInt(field) + 1
            }) }
            val client = lost(fixture); var rejected = false
            try { client.observe() } catch (_: Exception) { rejected = true }
            assertTrue("Invalid observed page/child remains unadopted: $field", rejected && !fixture.calls.contains("claude.response.forget"))
        }
    }
    @Test fun invalidationRejectsObservationBeforeAndAfterAwait() = main { supervisorScope {
        var allRejected = true
        for (stage in listOf("claude.status", "claude.response.read")) {
            val fixture = ClaudeObservationFixture(); val client = lost(fixture); fixture.pause = stage
            val task = async { client.observe() }; withTimeout(5000) { fixture.entered.await() }
            client.invalidate(); fixture.release.complete(Unit)
            var rejected = false; try { task.await() } catch (_: Exception) { rejected = true }
            val count = fixture.calls.size
            try { client.observe(); rejected = false } catch (_: Exception) { }
            allRejected = allRejected && rejected && count == fixture.calls.size
        }
        assertTrue("Detached Runtime rejects late and new reads", allRejected)
    } }
    @Test fun busyAndCancelledObserversCannotOverlapOrDispatch() = main { supervisorScope {
        val fixture = ClaudeObservationFixture(); val client = lost(fixture); fixture.pause = "claude.status"
        val task = async { client.observe() }; withTimeout(5000) { fixture.entered.await() }
        var busy = false; try { client.observe() } catch (_: IllegalStateException) { busy = true }
        task.cancel(); fixture.release.complete(Unit)
        var cancelled = false; try { task.await() } catch (_: CancellationException) { cancelled = true }
        assertTrue(busy && cancelled && fixture.calls.count { it == "claude.call" } == 1 && !fixture.calls.contains("claude.response.read"))
    } }
    @Test fun failedNewOpenCannotReadPreviousIdentityOrCache() = main {
        val fixture = ClaudeObservationFixture().apply { failurePoint = "" }; val client = ClaudeRPCClient(fixture::rpc)
        client.request("poll", "{}".toByteArray()); fixture.failurePoint = "open"
        try { client.request("send", "{}".toByteArray()); fail("Expected failed open") } catch (_: Exception) { }
        val before = fixture.calls.size; val result = client.observe()
        assertTrue("New upload cannot adopt an old result", result.source == ClaudeRPCClient.Observation.Source.UNAVAILABLE &&
            result.inputID == null && result.response == null && before == fixture.calls.size)
    }
    @Test fun malformedExitObservationCannotAuthorizeRead() = main {
        for (value in listOf("", "null,null", "{\"state\":1,\"state\":2}", "null,\"other\":1", "null trailing")) {
            val fixture = ClaudeObservationFixture().apply { exitJSON = value }; val client = lost(fixture)
            var rejected = false; try { client.observe() } catch (_: Exception) { rejected = true }
            assertTrue(rejected && !fixture.calls.contains("claude.response.read"))
        }
    }
    @Test fun liveAdapterAndInactiveViewModelCannotObserve() = main {
        val fixture = ClaudeObservationFixture(); val client = ClaudeRPCClient(fixture::rpc)
        var rejected = false; try { client.observe() } catch (_: Exception) { rejected = true }
        assertTrue(rejected && fixture.calls.isEmpty())
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val model = ExtensionPackagesViewModel(context.applicationContext as android.app.Application)
        rejected = false; try { model.observeClaudeOutcome() } catch (_: Exception) { rejected = true }
        assertTrue(rejected)
    }
    @Test fun observedByteArraysCannotMutateTheRetainedRecord() = main {
        val fixture = ClaudeObservationFixture(); val client = lost(fixture); val observed = client.observe()
        observed.response!!.fill(0)
        assertTrue("Caller arrays cannot edit the observation", observed.text == fixture.body.toString(Charsets.UTF_8) && client.observe().response!!.contentEquals(fixture.body))
    }
}

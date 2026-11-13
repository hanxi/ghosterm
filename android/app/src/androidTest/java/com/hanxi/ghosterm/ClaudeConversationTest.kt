package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.util.Locale

/** Explicit in-memory Native action fixture, not an SDK or a model. */
internal class ClaudeChatFixture {
    data class Call(val method: String, val input: JSONObject, val raw: String)
    val calls = mutableListOf<Call>()
    val events = ArrayDeque<JSONObject>()
    var phase = "new"
    var inputID = "input-one"
    var sequence = 0
    var rejectSend = false
    var failSend = false
    var accepted = true
    var readyPhase = "ready"
    var sendEntered: CompletableDeferred<Unit>? = null
    var sendRelease: CompletableDeferred<Unit>? = null
    var pollEntered: CompletableDeferred<Unit>? = null
    var pollRelease: CompletableDeferred<Unit>? = null
    var decisionEntered: CompletableDeferred<Unit>? = null
    var decisionRelease: CompletableDeferred<Unit>? = null
    suspend fun request(method: String, bytes: ByteArray): ByteArray {
        val input = ClaudeJson.objectValue(bytes); calls += Call(method, input, bytes.toString(Charsets.UTF_8))
        if (method == "send") { sendEntered?.complete(Unit); sendRelease?.await() }
        if (method == "poll") { pollEntered?.complete(Unit); pollRelease?.await() }
        if (method in listOf("approve", "answer")) { decisionEntered?.complete(Unit); decisionRelease?.await() }
        if (method == "send" && failSend) error("uncertain fixture send")
        val result = when (method) {
            "start" -> { phase = readyPhase; JSONObject().put("requested", true).put("sdkVersion", "0.3.323").put("cliVersion", "2.1.323") }
            "send" -> { phase = "running"; JSONObject().put("messageID", inputID) }
            "approve", "answer" -> JSONObject().put("accepted", accepted)
            "interrupt" -> { phase = "interrupting"; JSONObject().put("requested", true) }
            "stop" -> { phase = "closed"; JSONObject().put("phase", "closed") }
            "poll" -> {
                val batch = JSONArray(); var used = 0
                while (events.isNotEmpty() && batch.length() < 4 && used < 4096) {
                    val event = events.removeFirst(); batch.put(event); used += ClaudeJson.utf8(event.toString()).size
                }
                JSONObject().put("source", "batch").put("events", batch).put("phase", phase)
            }
            else -> error("Unexpected fixture action $method")
        }
        val value = if (method == "send" && rejectSend) JSONObject().put("ok", false).put("error", "ACTION_REJECTED")
            else JSONObject().put("ok", true).put("result", result)
        return ClaudeJson.utf8(value.toString())
    }
    fun message(message: JSONObject) = session(JSONObject().put("kind", "sdk-message").put("message", message))
    fun session(event: JSONObject) { events += JSONObject().put("source", "session").put("event", event.put("sequence", ++sequence)) }
    fun result(id: String = inputID, error: Boolean = false) {
        phase = "ready"
        message(JSONObject().put("type", "result").put("user_message_uuid", id).put("is_error", error)
            .put("subtype", if (error) "error_during_execution" else "success").put("result", if (error) "fixture backend error" else "fixture result"))
    }
    fun prompt(name: String = "request-one", token: String = "token-one", questions: JSONArray? = null, turn: String = inputID) {
        val input = if (questions != null) JSONObject().put("questions", questions) else JSONObject().put("command", "fixture only; " + "中😀".repeat(1200))
        val request = JSONObject().put("reference", reference(name, token).json()).put("kind", if (questions == null) "tool" else "question")
            .put("toolName", if (questions == null) "Bash" else "AskUserQuestion").put("turnID", turn).put("input", input)
        if (questions != null) request.put("questions", questions)
        events += JSONObject().put("source", "approval").put("event", JSONObject().put("kind", "requested").put("request", request))
    }
    fun expire(name: String = "request-one") { events += JSONObject().put("source", "approval").put("event",
        JSONObject().put("kind", "expired").put("reference", reference(name).json())) }
    fun reference(name: String = "request-one", token: String = "token-one") = ClaudeConversation.Reference("fixture", name, token)
    fun question(name: String, multi: Boolean = false) = JSONObject().put("question", name).put("header", "选择").put("multiSelect", multi)
        .put("options", JSONArray().put(JSONObject().put("label", "Blue").put("description", "First"))
            .put(JSONObject().put("label", "Green").put("description", "Second")))
    fun release() { sendRelease?.complete(Unit); pollRelease?.complete(Unit); decisionRelease?.complete(Unit) }
}

@RunWith(AndroidJUnit4::class)
class ClaudeConversationTest {
    private fun main(block: suspend CoroutineScope.() -> Unit) = runBlocking { withContext(Dispatchers.Main.immediate) { block() } }
    private suspend fun until(predicate: () -> Boolean) {
        val end = android.os.SystemClock.elapsedRealtime() + 5000
        while (!predicate() && android.os.SystemClock.elapsedRealtime() < end) delay(5)
        assertTrue("Expected Native Claude state", predicate())
    }
    private suspend fun fixture(block: suspend (ClaudeConversation, ClaudeChatFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        val fake = ClaudeChatFixture(); val model = ClaudeConversation(scope, fake::request)
        try {
            assertTrue(fake.calls.isEmpty()); model.start(); until { model.state.value.phase == ClaudeConversation.Phase.READY }
            block(model, fake)
        } finally { model.invalidate(); fake.release(); scope.cancel() }
    }

    @Test fun explicitInputStreamingAndCorrelatedResultsRemainDistinct() = main { fixture { model, fake ->
        model.editDraft("literal input 中文😀\n"); model.send(); until { model.state.value.phase == ClaudeConversation.Phase.RUNNING }
        assertEquals("literal input 中文😀\n", fake.calls.first { it.method == "send" }.input.getString("text"))
        for (text in listOf("Hello ", "中文😀")) fake.message(JSONObject().put("type", "stream_event").put("event", JSONObject()
            .put("type", "content_block_delta").put("index", 0).put("delta", JSONObject().put("type", "text_delta").put("text", text))))
        model.pollOnce(); assertTrue(model.state.value.entries.any { it.text == "Hello 中文😀" })
        fake.message(JSONObject().put("type", "assistant").put("message", JSONObject().put("content", JSONArray()
            .put(JSONObject().put("type", "text").put("text", "Hello 中文😀")))))
        fake.result(); model.pollOnce()
        assertNull(model.state.value.activeMessageID); assertFalse(model.state.value.entries.any { it.id.startsWith("stream:") })
        fake.inputID = "input-two"; model.editDraft("second input"); model.send(); fake.result(error = true); model.pollOnce()
        assertTrue(model.state.value.entries.any { it.title == "本轮未成功" })
    } }

    @Test fun approvalIsExplicitAndAStaleReferenceCannotRunTwice() = main { fixture { model, fake ->
        model.editDraft("fixture"); model.send(); fake.prompt(); model.pollOnce()
        assertFalse(fake.calls.any { it.method == "approve" }); val ref = model.state.value.pending.single().reference
        model.respond(ref, true); until { model.state.value.pending.isEmpty() }; model.respond(ref, true)
        val calls = fake.calls.filter { it.method == "approve" }; assertEquals(1, calls.size)
        assertTrue(calls.single().input.getBoolean("allow")); assertFalse(calls.single().input.has("input"))
        fake.prompt("é"); model.pollOnce(); model.respond(fake.reference("e\u0301"), true)
        assertEquals("Byte-distinct references must stay separate", 1, fake.calls.count { it.method == "approve" })
    } }

    @Test fun questionsNeedEveryAnswerAndJavaScriptKeyOrder() = main { fixture { model, fake ->
        model.editDraft("question"); model.send()
        fake.prompt(questions = JSONArray(listOf("10", "2", "é", "e\u0301").map { fake.question(it) })); model.pollOnce()
        val ref = model.state.value.pending.single().reference
        model.respond(ref, true); model.answer(ref, listOf("one", "two", "", "four"))
        assertFalse(fake.calls.any { it.method in listOf("approve", "answer") })
        model.answer(ref, listOf("first", "second", "三/\n", "四")); until { model.state.value.pending.isEmpty() }
        val call = fake.calls.single { it.method == "answer" }
        assertTrue("Integer question keys must use JS order", call.raw.contains("\"answers\":{\"2\":\"second\",\"10\":\"first\""))
        assertTrue("Unicode question keys must keep exact spellings", call.input.getJSONObject("answers").has("é") && call.input.getJSONObject("answers").has("e\u0301"))
        assertEquals("三/\n", call.input.getJSONObject("answers").getString("é"))
        assertEquals("四", call.input.getJSONObject("answers").getString("e\u0301"))
        assertTrue("Slash must use JSON.stringify spelling", call.raw.contains("三/\\n"))
    } }

    @Test fun queuedDecisionRechecksExpiryAfterAnInFlightPoll() = main { fixture { model, fake ->
        model.editDraft("fixture"); model.send(); fake.prompt(); model.pollOnce()
        val ref = model.state.value.pending.single().reference
        fake.pollEntered = CompletableDeferred(); fake.pollRelease = CompletableDeferred()
        coroutineScope {
            val poll = launch { model.pollOnce() }; fake.pollEntered!!.await()
            model.respond(ref, true); fake.expire(); fake.pollRelease!!.complete(Unit); poll.join()
            until { !model.state.value.busy }
        }
        assertFalse("Expired queued decision must not dispatch", fake.calls.any { it.method == "approve" })
        assertTrue(model.state.value.pending.isEmpty())
    } }

    @Test fun interruptReceiptDoesNotCompleteTheInput() = main { fixture { model, fake ->
        model.editDraft("fixture"); model.send(); fake.prompt(); model.pollOnce(); val ref = model.state.value.pending.single().reference
        model.interrupt(); until { model.state.value.phase == ClaudeConversation.Phase.INTERRUPTING }
        assertTrue("Interrupt must expire controls", model.state.value.pending.isEmpty())
        fake.session(JSONObject().put("kind", "interrupt-receipt").put("messageID", fake.inputID).put("receipt", JSONObject().put("requested", true)))
        model.pollOnce(); assertEquals("input-one", model.state.value.activeMessageID); assertFalse(model.canSend)
        model.respond(ref, true); assertFalse(fake.calls.any { it.method == "approve" })
        fake.result(); model.pollOnce(); assertTrue(model.canSend)
    } }

    @Test fun foreignResultsAndWrongTurnPromptsKeepTheInputUnknown() = main {
        for (prompt in listOf(false, true)) fixture { model, fake ->
            model.editDraft("fixture"); model.send()
            if (prompt) fake.prompt(turn = "other-input") else fake.result(id = "other-input")
            model.pollOnce(); assertEquals("input-one", model.state.value.activeMessageID)
            assertTrue(model.state.value.pending.isEmpty()); assertFalse(model.canSend)
            assertEquals(if (prompt) ClaudeConversation.Phase.UNKNOWN else ClaudeConversation.Phase.LOST, model.state.value.phase)
        }
    }

    @Test fun ownerChangeRejectsLateSendAndKeepsTheDraft() = main {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val fake = ClaudeChatFixture(); var active = true
        val model = ClaudeConversation(scope, fake::request, { active })
        try {
            model.start(); until { model.canSend }; fake.sendEntered = CompletableDeferred(); fake.sendRelease = CompletableDeferred()
            model.editDraft("keep literal draft\n"); model.send(); fake.sendEntered!!.await()
            active = false; fake.sendRelease!!.complete(Unit); until { !model.state.value.busy }
            assertEquals("Late send cannot replace the unknown draft", "keep literal draft\n", model.state.value.draft)
            assertNull(model.state.value.activeMessageID); assertEquals(ClaudeConversation.Phase.UNKNOWN, model.state.value.phase)
        } finally { model.invalidate(); fake.release(); scope.cancel() }
    }

    @Test fun uncertainSendNeverRetriesAndKnownRejectionStaysVisible() = main {
        for (uncertain in listOf(false, true)) fixture { model, fake ->
            fake.failSend = uncertain; fake.rejectSend = !uncertain
            model.editDraft("keep me"); model.send(); until { !model.state.value.busy }
            assertEquals("keep me", model.state.value.draft); assertNotNull(model.state.value.issue)
            if (uncertain) { model.send(); assertEquals(1, fake.calls.count { it.method == "send" }); assertFalse(model.canSend) }
        }
    }

    @Test fun closedStartupIsVisibleWithoutAnyInputAndLocaleDoesNotChangePhases() = main {
        val locale = Locale.getDefault()
        try {
            Locale.setDefault(Locale.forLanguageTag("tr-TR"))
            for (phase in listOf("failed", "lost")) {
                val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val fake = ClaudeChatFixture(); fake.readyPhase = phase
                val model = ClaudeConversation(scope, fake::request)
                try {
                    model.start(); model.pollOnce(); assertNotNull("Startup failure must be visible", model.state.value.issue)
                    assertNull(model.state.value.activeMessageID)
                    assertEquals(if (phase == "failed") ClaudeConversation.Phase.FAILED else ClaudeConversation.Phase.LOST, model.state.value.phase)
                }
                finally { model.invalidate(); scope.cancel() }
            }
        } finally { Locale.setDefault(locale) }
    }

    @Test fun historyCountsBytesAndKeepsEmptyPollsOutOfRawRecords() = main { fixture { model, fake ->
        repeat(3) { model.pollOnce() }; assertTrue(model.state.value.records.isEmpty())
        repeat(12) { fake.message(JSONObject().put("type", "system").put("detail", "大".repeat(100000))); model.pollOnce() }
        assertTrue("Raw record byte budget must evict", model.state.value.omittedRecords > 0)
        assertTrue("Entry byte budget must evict", model.state.value.omittedEntries > 0)
        assertTrue(model.state.value.records.sumOf { ClaudeJson.utf8(it.json).size } <= 2097152)
        assertTrue(model.state.value.entries.sumOf { ClaudeJson.utf8(it.text).size } <= 2097152)
        repeat(140) { fake.message(JSONObject().put("type", "system").put("detail", "small")); model.pollOnce() }
        assertTrue("Raw record count must be bounded", model.state.value.records.size <= 128)
        assertTrue("Visible entry count must be bounded", model.state.value.entries.size <= 128)
    } }

    @Test fun outOfOrderEventsFailObservationAndExpiredDecisionIsNotRetried() = main {
        fixture { model, fake ->
            fake.sequence = 1; fake.message(JSONObject().put("type", "system")); model.pollOnce()
            assertEquals("Sequence gaps must end observation", ClaudeConversation.Phase.UNKNOWN, model.state.value.phase)
        }
        fixture { model, fake ->
            model.editDraft("fixture"); model.send(); fake.prompt(); model.pollOnce(); fake.accepted = false
            val ref = model.state.value.pending.single().reference; model.respond(ref, true); until { model.state.value.pending.isEmpty() }
            assertNotNull(model.state.value.issue); model.respond(ref, true); assertEquals(1, fake.calls.count { it.method == "approve" })
        }
    }

    @Test fun finishedTurnPromptsAndInterruptReceiptsCannotAffectANewInput() = main { fixture { model, fake ->
        model.editDraft("one"); model.send(); fake.result(); model.pollOnce()
        fake.inputID = "input-two"; model.editDraft("two"); model.send()
        fake.prompt(turn = "input-one"); fake.session(JSONObject().put("kind", "interrupt-receipt").put("messageID", "input-one"))
        model.pollOnce(); assertEquals("input-two", model.state.value.activeMessageID); assertTrue(model.state.value.pending.isEmpty())
        assertEquals(ClaudeConversation.Phase.RUNNING, model.state.value.phase)
    } }

    @Test fun explicitStopRetainsUnconfirmedInputAndInvalidationDisablesControls() = main { fixture { model, fake ->
        model.editDraft("fixture"); model.send(); fake.prompt(); model.pollOnce(); model.stop()
        until { model.state.value.phase == ClaudeConversation.Phase.CLOSED }
        assertEquals("input-one", model.state.value.activeMessageID); assertNotNull(model.state.value.issue)
        assertTrue(model.state.value.pending.isEmpty()); model.invalidate(); assertEquals(ClaudeConversation.Phase.CLOSED, model.state.value.phase)
        assertFalse(model.canSend || model.canStop || model.canStart)
    } }

    @Test fun questionProjectionCannotChangeTheOriginalInput() = main { fixture { model, fake ->
        model.editDraft("fixture"); model.send(); fake.prompt(questions = JSONArray().put(fake.question("original")))
        fake.events.last().getJSONObject("event").getJSONObject("request").put("questions", JSONArray().put(fake.question("changed")))
        model.pollOnce()
        assertTrue("Mismatched original question input cannot offer a decision", model.state.value.pending.isEmpty())
        assertEquals(ClaudeConversation.Phase.UNKNOWN, model.state.value.phase)
    } }
}

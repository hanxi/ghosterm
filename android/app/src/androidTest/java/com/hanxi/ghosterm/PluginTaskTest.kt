package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

internal class PluginTaskFixture {
    val description = JSONObject().put("viewId", "task-a").put("revision", "18446744073709551615").put("title", "Prime task").put("unit", "candidates").put("cancellable", true)
    val state = JSONObject().put("viewId", "task-a").put("revision", "18446744073709551615").put("sequence", "1").put("phase", "running").put("completed", "4")
        .put("total", JSONObject().put("kind", "known").put("units", "100")).put("message", "Counting primes").put("result", JSONObject().put("kind", "none"))
    val calls = mutableListOf<Pair<String, JSONObject>>()
    var unknownMethod: String? = null
    var failureMethod: String? = null
    var cancelledRPC = false
    var badAck = false
    var pause: String? = null
    val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
    fun outcome(value: JSONObject) = JSONObject().put("kind", "success").put("value", value).toString().toByteArray(Charsets.UTF_8)
    suspend fun request(method: String, bytes: ByteArray): ByteArray {
        val input = JSONObject(bytes.toString(Charsets.UTF_8)); calls += method to input
        val result = when {
            unknownMethod == method -> """{"kind":"unknown","reason":"UNCONFIRMED"}""".toByteArray()
            failureMethod == method -> if (cancelledRPC) """{"kind":"cancelled","effect":"possible"}""".toByteArray()
                else """{"kind":"failed","error":{"code":"DENIED","message":"Explicit refusal","effect":"none"}}""".toByteArray()
            method == "view.task.describe" -> outcome(description)
            method == "view.task.read" -> outcome(state)
            else -> outcome(JSONObject().put("viewId", description.getString("viewId")).put("revision", description.getString("revision"))
                .put("requestId", if (badAck) "other" else input.getString("requestId")).put("kind", "requested"))
        }
        if (pause == method) { pause = null; entered.complete(Unit); withContext(NonCancellable) { release.await() } }
        return result
    }
    fun finish(phase: String = "cancelled") {
        state.put("sequence", "2").put("phase", phase)
        state.put("result", if (phase == "cancelled") JSONObject().put("kind", "cancelled").put("effect", "none") else JSONObject().put("kind", "success").put("summary", "25 primes"))
        if (phase == "succeeded") state.put("completed", "100")
    }
}

@RunWith(AndroidJUnit4::class)
class PluginTaskTest {
    private fun main(block: suspend CoroutineScope.() -> Unit) = runBlocking { withContext(Dispatchers.Main.immediate) { block() } }
    private fun rejected(block: () -> Unit) { try { block(); fail("Expected task value rejection") } catch (_: Exception) { } }
    private fun description(f: PluginTaskFixture) = PluginTaskCodec.descriptor(f.outcome(f.description))
    private fun snapshot(f: PluginTaskFixture, previous: PluginTaskSnapshot? = null) = PluginTaskCodec.snapshot(f.outcome(f.state), description(f), previous)
    @Test fun descriptorIdentityBooleanAndUnicodeAreStrict() {
        val f = PluginTaskFixture(); f.description.put("title", "😀".repeat(128)); assertEquals(128, description(f).title.codePointCount(0, 256))
        for ((key, value) in listOf("viewId" to "bad\n", "cancellable" to 1, "unit" to "", "extra" to 0, "title" to "e\u0301".repeat(65))) {
            rejected { PluginTaskCodec.descriptor(f.outcome(JSONObject(f.description.toString()).put(key, value))) }
        }
    }
    @Test fun revisionIsCanonicalPositiveUint64() {
        val f = PluginTaskFixture(); assertEquals("18446744073709551615", description(f).revision)
        for (bad in listOf("0", "01", "+1", "1\n", "18446744073709551616", 1)) rejected { description(f.apply { description.put("revision", bad) }) }
    }
    @Test fun snapshotBindsView() { val f = PluginTaskFixture(); f.state.put("viewId", "other"); rejected { snapshot(f) } }
    @Test fun snapshotBindsRevision() { val f = PluginTaskFixture(); f.state.put("revision", "1"); rejected { snapshot(f) } }
    @Test fun exactCountersAndUnknownZeroRemainDistinct() {
        val f = PluginTaskFixture(); f.state.put("completed", "18446744073709551615").put("total", JSONObject().put("kind", "unknown"))
        assertEquals("18446744073709551615", snapshot(f).completed); assertNull(snapshot(f).total)
        for (bad in listOf(0, "00", "-1", "1.0", "18446744073709551616")) rejected { snapshot(f.apply { state.put("completed", bad) }) }
        f.state.put("completed", "0").put("total", JSONObject().put("kind", "known").put("units", "0")); assertEquals("0", snapshot(f).total)
        f.state.put("total", JSONObject().put("kind", "unknown").put("units", "0")); rejected { snapshot(f) }
    }
    @Test fun phaseResultCompleteProgressAndCancellationCapabilityMatch() {
        val f = PluginTaskFixture(); f.state.put("phase", "succeeded").put("result", JSONObject().put("kind", "success").put("summary", "done"))
        rejected { snapshot(f) }; f.state.put("completed", "100"); assertEquals(PluginTaskSnapshot.Phase.SUCCEEDED, snapshot(f).phase)
        f.state.put("phase", "running"); rejected { snapshot(f) }; f.description.put("cancellable", false); f.finish(); rejected { snapshot(f) }
    }
    @Test fun sequenceCannotGoBack() { val f = PluginTaskFixture(); f.state.put("sequence", "2"); val old = snapshot(f); f.state.put("sequence", "1"); rejected { snapshot(f, old) } }
    @Test fun sameSequenceCannotChange() { val f = PluginTaskFixture(); val old = snapshot(f); f.state.put("message", "changed"); rejected { snapshot(f, old) } }
    @Test fun progressCannotGoBack() { val f = PluginTaskFixture(); val old = snapshot(f); f.state.put("sequence", "2").put("completed", "3"); rejected { snapshot(f, old) } }
    @Test fun knownTotalCannotChange() { val f = PluginTaskFixture(); val old = snapshot(f); f.state.put("sequence", "2").put("total", JSONObject().put("kind", "unknown")); rejected { snapshot(f, old) } }
    @Test fun terminalCannotBeRewritten() {
        val f = PluginTaskFixture(); f.finish(); val old = snapshot(f); f.state.put("sequence", "3").put("phase", "running").put("result", JSONObject().put("kind", "none")); rejected { snapshot(f, old) }
    }
    @Test fun strictRawJsonAndAggregateBytes() {
        val f = PluginTaskFixture(); val normal = f.outcome(f.description).toString(Charsets.UTF_8)
        for (bad in listOf(normal + "{}", normal.replace("\"unit\":\"candidates\"", "\"unit\":\"candidates\",\"unit\":\"candidates\""), normal.replace("Prime task", "\\ud800"))) rejected { PluginTaskCodec.descriptor(bad.toByteArray()) }
        f.description.put("viewId", "a".repeat(64)); f.state.put("viewId", "a".repeat(64)).put("phase", "failed").put("message", "😀".repeat(128))
            .put("result", JSONObject().put("kind", "failed").put("code", "😀".repeat(64)).put("message", "😀".repeat(256)))
        rejected { snapshot(f) }
    }
    @Test fun cancelAckIdentityDoesNotAllowReplay() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); f.badAck = true; m.cancel()!!.join()
        assertTrue(m.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED && !m.canCancel && !m.canAutoRefresh && m.state.value.cancelRequestID != null)
    }
    @Test fun cancelNeverRepeatsAfterUnknown() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); f.unknownMethod = "view.task.cancel"; m.cancel()!!.join(); m.cancel()?.join()
        assertEquals(1, f.calls.count { it.first == "view.task.cancel" })
    }
    @Test fun explicitReadDoesNotRewriteUnknownCancel() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); f.unknownMethod = "view.task.cancel"; m.cancel()!!.join(); val id = m.state.value.cancelRequestID
        f.finish(); m.load()!!.join(); m.cancel()?.join()
        assertTrue(m.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.CANCELLED && m.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED && m.state.value.cancelRequestID == id)
    }
    @Test fun acknowledgementDoesNotConfirmStop() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); m.cancel()!!.join()
        assertTrue(m.state.value.cancellation == PluginTaskModel.Cancellation.REQUESTED && m.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.RUNNING && !m.canCancel)
        f.finish(); m.load()!!.join(); assertEquals(PluginTaskSnapshot.Phase.CANCELLED, m.state.value.snapshot?.phase)
    }
    @Test fun declaredFailureAndCancelledRequestStayDistinct() = main {
        for (cancelled in listOf(false, true)) {
            val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); f.failureMethod = "view.task.cancel"; f.cancelledRPC = cancelled; m.cancel()!!.join()
            assertEquals(if (cancelled) PluginTaskModel.Cancellation.CANCELLED else PluginTaskModel.Cancellation.FAILED, m.state.value.cancellation)
            assertEquals(PluginTaskSnapshot.Phase.RUNNING, m.state.value.snapshot?.phase); assertFalse(m.canAutoRefresh)
        }
    }
    @Test fun longWireFailureAndControlsKeepDeclaredOutcome() {
        for (message in listOf("😀".repeat(1024), "bad\u0000message")) {
            val data = JSONObject().put("kind", "failed").put("error", JSONObject().put("code", "DENIED").put("message", message).put("effect", "possible")).toString().toByteArray()
            var preserved = false
            try { PluginTaskCodec.descriptor(data) } catch (failure: PluginTaskCodec.RPCFailure) { preserved = failure.code == "DENIED" && failure.effect == "possible" && failure.detail == message.replace("\u0000", "\\u0000") }
            assertTrue(preserved)
        }
    }
    @Test fun unknownReadKeepsLastStateAndCanRecover() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); val old = m.state.value.snapshot
        f.unknownMethod = "view.task.read"; m.load()!!.join(); assertTrue(m.state.value.snapshot == old && m.state.value.issue != null && !m.canAutoRefresh)
        f.unknownMethod = null; f.finish("succeeded"); m.load()!!.join(); assertTrue(m.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.SUCCEEDED && !m.canCancel && !m.canAutoRefresh)
        m.invalidate(); assertTrue(m.state.value.issue!!.contains("已确认") && m.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.SUCCEEDED)
    }
    @Test fun lateReplyCannotChangeInvalidatedTask() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); val old = m.state.value.snapshot; f.finish(); f.pause = "view.task.read"
        val read = m.load()!!; withTimeout(5000) { f.entered.await() }; m.invalidate(); f.release.complete(Unit); read.join()
        assertTrue(!m.state.value.valid && !m.state.value.busy && m.state.value.snapshot == old)
    }
    @Test fun cancelledCallerKeepsActionUnconfirmed() = main {
        val f = PluginTaskFixture(); val m = PluginTaskModel(this, f::request); m.load()!!.join(); f.pause = "view.task.cancel"
        val action = m.cancel()!!; withTimeout(5000) { f.entered.await() }; action.cancel(); f.release.complete(Unit); action.join()
        assertTrue(m.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED && m.state.value.issue != null && !m.canCancel && !m.canAutoRefresh)
    }
    @Test fun busyAndReplacedInstanceCannotStartMoreRequests() = main {
        val f = PluginTaskFixture(); var current = true; val m = PluginTaskModel(this, f::request) { current }; f.pause = "view.task.describe"
        val read = m.load()!!; withTimeout(5000) { f.entered.await() }; assertNull(m.load()); assertNull(m.cancel()); current = false; f.release.complete(Unit); read.join()
        assertTrue(m.state.value.descriptor == null && m.state.value.snapshot == null && f.calls.size == 1)
    }
    @Test fun cancelledOwnerDoesNotStartAnAction() = main {
        val f = PluginTaskFixture(); val owner = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val m = PluginTaskModel(owner, f::request); m.load()!!.join(); owner.cancel()
        assertNull(m.cancel()); assertNull(m.load()); assertEquals(2, f.calls.size)
    }
    @Test fun nativeBootstrapUsesOnlyUniqueImplementedServices() {
        val hash = "a".repeat(64); val request = ExtensionRelayBootstrap.Request("/gterm", "/package", hash, hash, PluginTaskModel.methods, command = "task primes", clientCapabilities = listOf("ui.task"))
        assertTrue(ExtensionRelayBootstrap.build(request).contains("'--client-capability' 'ui.task'"))
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.task", "ui.task"))) }
        rejected { ExtensionRelayBootstrap.build(request.copy(clientCapabilities = listOf("ui.logs"))) }
    }
}

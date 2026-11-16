package com.hanxi.ghosterm

import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@RunWith(AndroidJUnit4::class)
class PluginPolicyReviewModelTest {
    private val document get() = PluginSignatureDocument("bounded policy snapshot".toByteArray())
    private fun value(id: String) = PluginPolicyReview.Value(id, "a".repeat(64), "fixture", document.sha256, listOf(
        PluginPolicyReview.Entry("org.ghosterm.accepted", "b".repeat(64), PluginPolicyReview.State.ACCEPTED, "accepted"),
        PluginPolicyReview.Entry("org.ghosterm.rejected", "c".repeat(64), PluginPolicyReview.State.REJECTED, "rejected"),
        PluginPolicyReview.Entry("org.ghosterm.unknown", "d".repeat(64), PluginPolicyReview.State.UNKNOWN, "unknown")))
    private fun result(ids: List<String>, durable: Boolean = true) = ExtensionController.PolicyDisableResult(ids,
        PluginReleaseCatalog.Observation(emptyList(), emptyList(), 0, PluginReleaseCatalog.Selection(null, emptyMap()), null), durable)
    private suspend fun <T> main(block: () -> T) = withContext(Dispatchers.Main.immediate) { block() }
    private suspend fun idle(m: PluginPolicyReviewModel) = withTimeout(3000) { while (main { m.busy }) delay(5) }
    private class Harness { val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); var inspections = 0; val commits = mutableListOf<List<String>>(); val cancels = mutableListOf<String>(); var changes = 0 }
    private fun model(h: Harness, inspect: suspend (String, ByteArray) -> PluginPolicyReview.Value = { id, _ -> h.inspections++; value(id) },
        commit: suspend (String, List<String>, ByteArray) -> ExtensionController.PolicyDisableResult = { _, ids, _ -> h.commits += ids; result(ids) }) =
        PluginPolicyReviewModel(h.scope, inspect, commit, { h.cancels += it }, { h.changes++ })
    private suspend fun ready(m: PluginPolicyReviewModel) { main { m.setPolicy(document, true); m.inspect() }; idle(m) }
    @Test fun trustIsRequiredAndOnlyExplicitUnacceptedSubsetCanBeCommitted() = runBlocking<Unit> {
        val h = Harness(); val m = model(h)
        try {
            main { m.inspect(); m.apply(); m.setPolicy(document, false); m.inspect() }; assertEquals(0, h.inspections)
            ready(m); assertTrue(main { m.selectedIDs.isEmpty() }); main { m.apply() }; assertTrue(h.commits.isEmpty())
            main { m.choose("org.ghosterm.accepted", true); m.choose("org.ghosterm.unlisted", true); m.apply() }; assertTrue(h.commits.isEmpty())
            main { m.choose("org.ghosterm.rejected", true); m.apply() }; idle(m)
            assertEquals(listOf(listOf("org.ghosterm.rejected")), h.commits); assertEquals(1, h.changes); assertNull(main { m.review })
        } finally { main { m.close() }; h.scope.cancel() }
    }
    @Test fun cancelTrustAndPolicyChangesClearEverySelectionAndKnownTicket() = runBlocking<Unit> {
        val h = Harness(); val m = model(h)
        try {
            ready(m); val first = main { m.review!!.id }; main { m.choose("org.ghosterm.unknown", true); m.cancel() }
            assertTrue(first in h.cancels); assertTrue(main { m.selectedIDs.isEmpty() }); assertNull(main { m.review })
            main { m.inspect() }; idle(m); main { m.choose("org.ghosterm.rejected", true); m.setPolicy(document, false) }
            assertNull(main { m.review }); assertFalse(main { m.canReview }); assertFalse(main { m.canApply })
            ready(m); main { m.choose("org.ghosterm.rejected", true); m.setPolicy(PluginSignatureDocument("changed".toByteArray()), true) }
            assertNull(main { m.review }); assertTrue(main { m.selectedIDs.isEmpty() }); assertTrue(h.commits.isEmpty())
        } finally { main { m.close() }; h.scope.cancel() }
    }
    @Test fun closeCancelsKnownRequestAndRejectsLateInspectionResult() = runBlocking<Unit> {
        val h = Harness(); lateinit var continuation: Continuation<PluginPolicyReview.Value>; val entered = CompletableDeferred<String>()
        val m = model(h, inspect = { id, _ -> suspendCoroutine { continuation = it; entered.complete(id) } })
        try {
            main { m.setPolicy(document, true); m.inspect() }; val id = withTimeout(3000) { entered.await() }
            main { m.close(); continuation.resume(value(id)) }; main { }
            assertTrue(id in h.cancels); assertNull(main { m.review }); assertTrue(h.commits.isEmpty())
        } finally { main { m.close() }; h.scope.cancel() }
    }
    @Test fun commitFreezesPolicySubsetAndCancellationAndPreservesVisibleUnsyncedReceipt() = runBlocking<Unit> {
        val h = Harness(); val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>(); var bytes: ByteArray? = null
        val m = model(h, commit = { _, ids, policy -> h.commits += ids; bytes = policy; entered.complete(Unit); release.await(); result(ids, false) })
        try {
            ready(m); main { m.choose("org.ghosterm.unknown", true); m.apply() }; withTimeout(3000) { entered.await() }
            main { m.cancel(); m.setPolicy(null, false); m.choose("org.ghosterm.rejected", true); m.inspect(); m.apply() }
            assertEquals(listOf(listOf("org.ghosterm.unknown")), h.commits); assertArrayEquals(document.copyBytes(), bytes)
            release.complete(Unit); idle(m); val receipt = main { m.result }; assertNotNull("commit lost its visible receipt", receipt); assertFalse(receipt!!.durabilityConfirmed); assertEquals(1, h.changes)
        } finally { release.complete(Unit); main { m.close() }; h.scope.cancel() }
    }
    @Test fun lostReceiptClearsTicketAndCannotBlindlyApplyAgain() = runBlocking<Unit> {
        val h = Harness(); val m = model(h, commit = { _, ids, _ -> h.commits += ids; throw java.io.IOException("PRIVATE_DETAIL") })
        try {
            ready(m); main { m.choose("org.ghosterm.rejected", true); m.apply() }; idle(m)
            assertNull(main { m.review }); assertNotNull(main { m.issue }); assertFalse(main { m.issue!!.contains("PRIVATE_DETAIL") })
            main { m.apply() }; assertEquals(1, h.commits.size); assertEquals(0, h.changes)
        } finally { main { m.close() }; h.scope.cancel() }
    }
    @Test fun actualPolicyPickerAndTrustModelSynchronouslyInvalidateAuditBeforeAnyLateRead() = runBlocking<Unit> {
        val h = Harness(); val m = model(h)
        val picker = PluginVersionSelectionModel(h.scope, { document }, { _, _, _, _ -> error("unused") }, { error("unused") }, {}, { _, _ -> error("unused") }, {}, m::setPolicy)
        try {
            main { val pick = picker.beginPick()!!; picker.completePick(pick, Uri.parse("content://fixture/policy")) }
            withTimeout(3000) { while (main { picker.busy }) delay(5) }
            assertFalse(main { m.canReview }); main { picker.trust(true); m.inspect() }; idle(m)
            main { m.choose("org.ghosterm.rejected", true); picker.beginPick() }
            assertNull(main { m.review }); assertTrue(main { m.selectedIDs.isEmpty() }); assertFalse(main { m.canReview })
        } finally { main { picker.close(); m.close() }; h.scope.cancel() }
    }
}

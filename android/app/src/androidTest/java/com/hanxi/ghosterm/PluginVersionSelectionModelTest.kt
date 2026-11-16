package com.hanxi.ghosterm

import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.util.UUID
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

/** Explicit trust, confirmation and page-lifetime races; no native substitute. */
@RunWith(AndroidJUnit4::class)
class PluginVersionSelectionModelTest {
    private val uri = Uri.parse("content://explicit.fixture/public-policy")
    private fun value(id: String = UUID.randomUUID().toString()): ExtensionController.VersionPreview {
        val snapshot = ExtensionController.SignedArchiveReview(
            ExtensionController.Review("org.ghosterm.fixture", "2.0.0", "fixture", "MIT", "a".repeat(64), "portable-js", listOf("session.rpc"), emptyList(), false, false, emptyList()),
            PluginReleaseSignature.Proof("org.ghosterm.fixture", "a".repeat(64), "fixture", "b".repeat(64), "fixture", "c".repeat(64), "d".repeat(64), "e".repeat(64), 32),
            PluginReleaseDeclarations.Data("f".repeat(64), "^1.0.0", "1.0.0", listOf(1), listOf(1), emptyList(), null))
        return ExtensionController.VersionPreview(id, null, "e".repeat(64), snapshot, null, ExtensionController.PreviousVersionState.UNKNOWN, "f".repeat(64), true)
    }
    private fun result(durable: Boolean = true) = ExtensionController.VersionSelectionResult("org.ghosterm.fixture", value().candidate.review,
        PluginReleaseCatalog.Observation(emptyList(), emptyList(), 0, PluginReleaseCatalog.Selection("a".repeat(64), emptyMap()), null), durable)
    private suspend fun <T> main(body: () -> T) = withContext(Dispatchers.Main.immediate) { body() }
    private suspend fun idle(model: PluginVersionSelectionModel) = withTimeout(3000) { while (main { model.busy }) delay(5) }
    private class Harness {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        var reads = 0; var inspections = 0; var commits = 0; var disables = 0; var changes = 0
        val cancelled = mutableListOf<String>()
    }
    private fun model(h: Harness,
        read: suspend (Uri) -> PluginSignatureDocument = { h.reads++; PluginSignatureDocument("public policy".toByteArray()) },
        inspect: suspend (String, String, String?, ByteArray) -> ExtensionController.VersionPreview = { id, _, _, _ -> h.inspections++; value(id) },
        commit: suspend (String) -> ExtensionController.VersionSelectionResult = { h.commits++; result() },
        disable: suspend (String, String?) -> ExtensionController.VersionSelectionResult = { _, _ -> h.disables++; result() }) =
        PluginVersionSelectionModel(h.scope, read, inspect, commit, { h.cancelled += it }, disable, { h.changes++ })
    private suspend fun policy(model: PluginVersionSelectionModel) {
        main { model.completePick(model.beginPick()!!, uri) }; idle(model)
    }
    private suspend fun preview(model: PluginVersionSelectionModel) {
        policy(model); main { model.trust(true); model.inspect("e".repeat(64), null) }; idle(model)
    }
    private suspend fun close(h: Harness, model: PluginVersionSelectionModel) { main { model.close() }; h.scope.cancel() }
    @Test fun policyTrustAndSeparatePreviewAcknowledgementAreRequiredBeforeCommit() = runBlocking {
        val h = Harness(); val model = model(h)
        try {
            main { model.inspect("e".repeat(64), null); model.acknowledge(true); model.confirm() }
            assertEquals(0, h.inspections); assertEquals(0, h.commits)
            policy(model); main { model.inspect("e".repeat(64), null) }; assertEquals(0, h.inspections)
            main { model.trust(true); model.inspect("e".repeat(64), null) }; idle(model)
            assertEquals(1, h.inspections); assertEquals(ExtensionController.PreviousVersionState.UNKNOWN, main { model.preview!!.previousState })
            main { model.confirm() }; assertEquals(0, h.commits)
            main { model.acknowledge(true); model.confirm() }; idle(model)
            assertEquals(1, h.commits); assertEquals(1, h.changes); assertNull(main { model.preview }); assertFalse(main { model.acknowledged })
            main { model.confirm() }; assertEquals(1, h.commits)
        } finally { close(h, model) }
    }
    @Test fun trustWithdrawalAndNewPolicyInvalidatePreviewAndReleaseKnownRequest() = runBlocking {
        val h = Harness(); val model = model(h)
        try {
            preview(model); val id = main { model.preview!!.id }; main { model.acknowledge(true); model.trust(false); model.confirm() }
            assertTrue(id in h.cancelled); assertNull(main { model.preview }); assertFalse(main { model.acknowledged }); assertEquals(0, h.commits)
            main { model.trust(true); model.inspect("e".repeat(64), null) }; idle(model)
            val next = main { model.preview!!.id }; policy(model)
            assertTrue(next in h.cancelled); assertFalse(main { model.trusted }); assertNull(main { model.preview })
        } finally { close(h, model) }
    }
    @Test fun latePolicyPickerReplyCannotReplaceANewerRequestOrClosedPage() = runBlocking {
        val h = Harness(); val model = model(h)
        try {
            val old = main { model.beginPick()!! }; main { model.failedPick(old) }
            val fresh = main { model.beginPick()!! }; main { model.completePick(old, uri) }
            assertEquals(0, h.reads); assertSame(fresh, main { model.pending })
            main { model.close(); model.completePick(fresh, uri) }; assertEquals(0, h.reads); assertNull(main { model.policy })
        } finally { close(h, model) }
    }
    @Test fun closingInFlightPreviewCancelsOwnerTokenBeforeLateReturnAndRejectsLateUi() = runBlocking {
        val h = Harness(); lateinit var continuation: Continuation<ExtensionController.VersionPreview>; lateinit var work: Job
        val entered = CompletableDeferred<String>()
        val model = model(h, inspect = { id, _, _, _ ->
            work = currentCoroutineContext()[Job]!!
            suspendCoroutine { continuation = it; entered.complete(id) }
        })
        try {
            policy(model); main { model.trust(true); model.inspect("e".repeat(64), null) }
            val id = withTimeout(3000) { entered.await() }; main { model.close() }
            assertTrue(work.isCancelled); assertTrue(id in h.cancelled)
            main { continuation.resume(value(id)) }; main { }
            assertNull(main { model.preview }); assertNull(main { model.issue }); assertEquals(0, h.changes)
            assertTrue(h.cancelled.count { it == id } >= 2)
        } finally { close(h, model) }
    }
    @Test fun commitFreezesMaterialAcknowledgementAndOtherActionsUntilPublishedReceipt() = runBlocking {
        val h = Harness(); val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        val model = model(h, commit = { h.commits++; entered.complete(Unit); release.await(); result(false) })
        try {
            preview(model); val original = main { model.preview!!.id }
            main { model.acknowledge(true); model.confirm() }; withTimeout(3000) { entered.await() }
            main { model.trust(false); model.cancel(); model.beginDisable("org.ghosterm.fixture", null); model.inspect("f".repeat(64), null); assertNull(model.beginPick()); model.acknowledge(false) }
            assertTrue(main { model.trusted }); assertTrue(main { model.acknowledged }); assertEquals(original, main { model.preview!!.id })
            release.complete(Unit); idle(model)
            assertEquals(1, h.commits); assertEquals(1, h.changes); assertFalse(main { model.result!!.durabilityConfirmed }); assertFalse(main { model.committing })
        } finally { release.complete(Unit); close(h, model) }
    }
    @Test fun disableHasItsOwnAcknowledgementAndCancelsCandidateWithoutRequiringPolicy() = runBlocking {
        val h = Harness(); val model = model(h)
        try {
            preview(model); val id = main { model.preview!!.id }
            main { model.acknowledge(true); model.beginDisable("org.ghosterm.fixture", "old-revision"); model.confirmDisable() }
            assertTrue(id in h.cancelled); assertNull(main { model.preview }); assertEquals(0, h.disables)
            main { model.acknowledge(true); model.confirmDisable() }; idle(model)
            assertEquals(1, h.disables); assertEquals(1, h.changes); assertNull(main { model.disableTarget }); assertFalse(main { model.acknowledged })
        } finally { close(h, model) }
    }
    @Test fun unknownCommitReceiptDoesNotGrantUiApprovalOrOfferUnconfirmedRetry() = runBlocking {
        val h = Harness(); val model = model(h, commit = { throw java.io.IOException("DO_NOT_SHOW_PRIVATE_DETAIL") })
        try {
            preview(model); main { model.acknowledge(true); model.confirm() }; idle(model)
            assertTrue(main { model.issue!!.contains("刷新列表核对") }); assertFalse(main { model.issue!!.contains("DO_NOT_SHOW") })
            assertNull(main { model.result }); assertNull(main { model.preview }); assertFalse(main { model.canConfirm }); assertEquals(0, h.changes)
        } finally { close(h, model) }
    }
    @Test fun closedPageIgnoresCommittedResultReturnedAfterCancellation() = runBlocking {
        val h = Harness(); lateinit var continuation: Continuation<ExtensionController.VersionSelectionResult>; val entered = CompletableDeferred<Unit>()
        val model = model(h, commit = { suspendCoroutine { continuation = it; entered.complete(Unit) } })
        try {
            preview(model); main { model.acknowledge(true); model.confirm() }; withTimeout(3000) { entered.await() }
            main { model.close(); continuation.resume(result()) }; main { }
            assertNull(main { model.result }); assertEquals(0, h.changes); assertFalse(main { model.committing })
        } finally { close(h, model) }
    }
}

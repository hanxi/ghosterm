package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@RunWith(AndroidJUnit4::class)
class PluginVersionRemovalModelTest {
    private fun view(id: String = "saved", pending: Boolean = false) = PluginReleaseRemoval.Preview(id, "a".repeat(64), "/owned/fixture", emptyList(), pending)
    private fun result(complete: Boolean = true) = PluginReleaseRemoval.Result("a".repeat(64), 3, 1, complete, complete)
    private suspend fun <T> main(block: () -> T) = withContext(Dispatchers.Main.immediate) { block() }
    private suspend fun idle(model: PluginVersionRemovalModel) = withTimeout(3000) { while (main { model.busy }) delay(5) }
    private class Harness {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        var reads = 0; var previews = 0; var commits = 0; var resumes = 0; var withdrawals = 0; var changes = 0
        var saved: PluginReleaseRemoval.Preview? = null
        val cancellations = mutableListOf<String>()
    }
    private fun model(h: Harness,
        read: suspend () -> PluginReleaseRemoval.Preview? = { h.reads++; h.saved },
        inspect: suspend (String, String, String?) -> PluginReleaseRemoval.Preview = { id, _, _ -> h.previews++; view(id) },
        commit: suspend (String) -> PluginReleaseRemoval.Result = { h.commits++; result() }) = PluginVersionRemovalModel(h.scope, read, inspect, commit,
        { h.cancellations += it }, { h.resumes++; h.saved = null; result() }, { h.withdrawals++; h.saved = null; true }, { h.changes++ })
    private suspend fun preview(m: PluginVersionRemovalModel) { main { m.refresh() }; idle(m); main { m.inspect("a".repeat(64), null) }; idle(m) }
    private suspend fun close(h: Harness, m: PluginVersionRemovalModel) { main { m.close() }; h.scope.cancel() }
    @Test fun observationNeverMutatesAndPreviewNeedsItsOwnFreshAcknowledgement() = runBlocking {
        val h = Harness(); val m = model(h)
        try {
            main { m.inspect("a".repeat(64), null); m.confirm(); m.resume(); m.withdraw() }
            assertEquals(0, h.previews); assertEquals(0, h.commits)
            preview(m); main { m.confirm() }; assertEquals(0, h.commits)
            main { m.acknowledge(true); m.confirm() }; idle(m)
            assertEquals(1, h.commits); assertEquals(1, h.changes); assertFalse(main { m.acknowledged }); assertNull(main { m.preview })
            assertEquals(0, h.resumes); assertEquals(0, h.withdrawals); assertEquals(3, main { m.result!!.removed })
        } finally { close(h, m) }
    }
    @Test fun corruptObservationDoesNotBecomeNoPendingIntentOrAllowPreview() = runBlocking {
        val h = Harness(); val m = model(h, read = { throw java.io.IOException("PRIVATE_DETAIL") })
        try {
            main { m.refresh() }; idle(m); main { m.inspect("a".repeat(64), null) }
            assertFalse(main { m.loaded }); assertFalse(main { m.canPreview }); assertEquals(0, h.previews)
            assertNotNull(main { m.issue }); assertFalse(main { m.issue!!.contains("PRIVATE_DETAIL") })
        } finally { close(h, m) }
    }
    @Test fun savedIntentRequiresExplicitAcknowledgedResumeOrWithdrawalAndIsNeverAutomaticallyReplayed() = runBlocking {
        for (withdraw in listOf(false, true)) {
            val h = Harness().apply { saved = view(pending = true) }; val m = model(h)
            try {
                main { m.refresh() }; idle(m); main { m.refresh() }; idle(m)
                assertEquals(0, h.resumes); assertEquals(0, h.withdrawals); assertFalse(main { m.canPreview })
                main { m.resume(); m.withdraw() }; assertEquals(0, h.resumes + h.withdrawals)
                main { m.acknowledge(true); if (withdraw) m.withdraw() else m.resume() }; idle(m)
                assertEquals(if (withdraw) 1 else 0, h.withdrawals); assertEquals(if (withdraw) 0 else 1, h.resumes)
                assertEquals(1, h.changes); assertFalse(main { m.acknowledged }); assertNull(main { m.saved })
            } finally { close(h, m) }
        }
    }
    @Test fun refreshAndCancelReleasePreviewAndResetAcknowledgementWithoutWriting() = runBlocking {
        val h = Harness(); val m = model(h)
        try {
            preview(m); val first = main { m.preview!!.id }
            main { m.acknowledge(true); m.refresh() }; idle(m)
            assertTrue(first in h.cancellations); assertFalse(main { m.acknowledged }); assertNull(main { m.preview })
            main { m.inspect("a".repeat(64), null) }; idle(m); val second = main { m.preview!!.id }
            main { m.cancel(); m.confirm() }
            assertTrue(second in h.cancellations); assertNull(main { m.preview }); assertEquals(0, h.commits)
        } finally { close(h, m) }
    }
    @Test fun closeCancelsKnownTokenBeforeLatePreviewAndRejectsLateResult() = runBlocking {
        val h = Harness(); lateinit var continuation: Continuation<PluginReleaseRemoval.Preview>
        val started = CompletableDeferred<String>()
        val m = model(h, inspect = { id, _, _ -> suspendCoroutine { continuation = it; started.complete(id) } })
        try {
            main { m.refresh() }; idle(m); main { m.inspect("a".repeat(64), null) }
            val id = withTimeout(3000) { started.await() }; main { m.close(); continuation.resume(view(id)) }; main { }
            assertTrue(h.cancellations.count { it == id } >= 2); assertNull(main { m.preview }); assertEquals(0, h.changes)
        } finally { close(h, m) }
    }
    @Test fun commitFreezesActionsAndPreservesPartialEffectReceiptAndSavedIntent() = runBlocking {
        val h = Harness(); val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        val m = model(h, commit = { h.commits++; entered.complete(Unit); release.await(); h.saved = view(pending = true); result(false) })
        try {
            preview(m); val original = main { m.preview!!.id }
            main { m.acknowledge(true); m.confirm() }; withTimeout(3000) { entered.await() }
            main { m.cancel(); m.refresh(); m.inspect("b".repeat(64), null); m.acknowledge(false); m.confirm() }
            assertEquals(original, main { m.preview!!.id }); assertTrue(main { m.acknowledged }); assertEquals(1, h.commits)
            release.complete(Unit); idle(m)
            assertFalse(main { m.result!!.complete }); assertEquals(3, main { m.result!!.removed }); assertNotNull(main { m.saved })
            assertFalse(main { m.canContinue }); assertEquals(0, h.resumes)
        } finally { release.complete(Unit); close(h, m) }
    }
    @Test fun lostReceiptRereadsOnlyAndDoesNotOfferBlindConfirmationRetry() = runBlocking {
        val h = Harness(); val m = model(h, commit = { h.saved = view(pending = true); throw java.io.IOException("PRIVATE_DETAIL") })
        try {
            preview(m); main { m.acknowledge(true); m.confirm() }; idle(m)
            assertNotNull(main { m.issue }); assertNotNull(main { m.saved }); assertNull(main { m.preview }); assertNull(main { m.result })
            assertEquals(0, h.resumes); assertEquals(0, h.changes); assertFalse(main { m.canContinue })
        } finally { close(h, m) }
    }
}

package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.util.UUID
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@RunWith(AndroidJUnit4::class)
class PluginInstallMaintenanceModelTest {
    private val ref = PluginInstallJournal.Reference(UUID.randomUUID().toString(), ".prepare-unused", "a".repeat(64), "b".repeat(64))
    private fun entry(state: PluginInstallJournal.State) = PluginInstallJournal.Entry("record", ref, state)
    private suspend fun <T> main(block: () -> T) = withContext(Dispatchers.Main.immediate) { block() }
    private suspend fun idle(m: PluginInstallMaintenanceModel) = withTimeout(3000) { while (main { m.busy }) delay(5) }
    @Test fun observationAndUnsafeRowsCannotRetireAnythingWithoutExplicitAck() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main); var calls = 0; var changes = 0
        val rows = PluginInstallJournal.State.entries.map(::entry)
        val m = PluginInstallMaintenanceModel(scope, { PluginInstallJournal.Observation(rows) }, { calls++; true }, { changes++ })
        try {
            main { m.refresh() }; idle(m); assertEquals(0, calls)
            for (row in rows.filter { it.state !in setOf(PluginInstallJournal.State.PUBLISHED, PluginInstallJournal.State.ABSENT) }) main { m.choose(row); assertNull(m.target) }
            main { m.choose(entry(PluginInstallJournal.State.PUBLISHED)); m.confirm() }; assertEquals(0, calls)
            main { m.acknowledge(true); m.confirm() }; idle(m)
            assertEquals(1, calls); assertEquals(1, changes); assertFalse(main { m.acknowledged }); assertNull(main { m.target })
        } finally { main { m.close() }; scope.cancel() }
    }
    @Test fun failureIsUnknownAndAnOldOrUnlistedReferenceCannotBeConfirmed() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main); var fail = false; var calls = 0
        val m = PluginInstallMaintenanceModel(scope, { if (fail) throw java.io.IOException("PRIVATE"); PluginInstallJournal.Observation(listOf(entry(PluginInstallJournal.State.ABSENT))) }, { calls++; true }, {})
        try {
            main { m.refresh() }; idle(m); main { m.choose(entry(PluginInstallJournal.State.ABSENT).copy(reference = ref.copy(digest = "f".repeat(64)))) }; assertNull(main { m.target })
            fail = true; main { m.refresh() }; idle(m)
            assertNull(main { m.observation }); assertNotNull(main { m.issue }); assertFalse(main { m.issue!!.contains("PRIVATE") })
            main { m.choose(entry(PluginInstallJournal.State.ABSENT)); m.acknowledge(true); m.confirm() }; assertEquals(0, calls)
        } finally { main { m.close() }; scope.cancel() }
    }
    @Test fun refreshWithdrawsTheAcknowledgementAndCommitKeepsReceiptDurabilitySeparate() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main); var calls = 0
        val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        val m = PluginInstallMaintenanceModel(scope, { PluginInstallJournal.Observation(listOf(entry(PluginInstallJournal.State.ABSENT))) }, { calls++; entered.complete(Unit); release.await(); false }, {})
        try {
            main { m.refresh() }; idle(m); main { m.choose(entry(PluginInstallJournal.State.ABSENT)); m.acknowledge(true); m.refresh() }; idle(m)
            assertNull(main { m.target }); assertFalse(main { m.acknowledged })
            main { m.choose(entry(PluginInstallJournal.State.ABSENT)); m.acknowledge(true); m.confirm() }; entered.await()
            main { m.cancel(); m.refresh(); m.acknowledge(false) }; assertTrue(main { m.acknowledged }); assertTrue(main { m.committing })
            release.complete(Unit); idle(m); assertEquals(1, calls); assertEquals(false, main { m.result }); assertFalse(main { m.acknowledged })
        } finally { release.complete(Unit); main { m.close() }; scope.cancel() }
    }
    @Test fun closingRejectsLateObservationAndNeverTurnsItIntoAnAction() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main); lateinit var continuation: Continuation<PluginInstallJournal.Observation>
        val entered = CompletableDeferred<Unit>(); var calls = 0
        val m = PluginInstallMaintenanceModel(scope, { suspendCoroutine { continuation = it; entered.complete(Unit) } }, { calls++; true }, {})
        try {
            main { m.refresh() }; entered.await(); main { m.close(); continuation.resume(PluginInstallJournal.Observation(listOf(entry(PluginInstallJournal.State.ABSENT)))) }; main { }
            assertNull(main { m.observation }); assertEquals(0, calls)
        } finally { main { m.close() }; scope.cancel() }
    }
}

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
class PluginInstalledVersionsModelTest {
    private fun value() = PluginReleaseCatalog.Observation(emptyList(), emptyList(), 0, PluginReleaseCatalog.Selection(null, emptyMap()), null)
    private suspend fun <T> main(body: () -> T): T = withContext(Dispatchers.Main.immediate) { body() }
    private suspend fun idle(model: PluginInstalledVersionsModel) = withTimeout(3000) { while (main { model.busy }) delay(5) }
    private fun scope() = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    @Test fun explicitRefreshClearsPreviousObservationAndRejectsOverlappingRequest() = runBlocking {
        val scope = scope(); var calls = 0; val gate = CompletableDeferred<Unit>()
        val model = PluginInstalledVersionsModel(scope) { calls++; if (calls > 1) gate.await(); value() }
        try {
            assertEquals(0, calls); main { model.refresh() }; idle(model); assertNotNull(main { model.observation })
            main { model.refresh(); model.refresh() }; assertNull(main { model.observation }); assertTrue(main { model.busy }); assertEquals(2, calls)
            gate.complete(Unit); idle(model); assertNotNull(main { model.observation })
        } finally { gate.complete(Unit); main { model.close() }; scope.cancel() }
    }
    @Test fun failureIsVisibleUnknownAndNeverAnEmptySuccessfulInventory() = runBlocking {
        val scope = scope(); val model = PluginInstalledVersionsModel(scope) { throw java.io.IOException("DO_NOT_SHOW_PRIVATE_DETAIL") }
        try {
            main { model.refresh() }; idle(model)
            assertNull(main { model.observation }); assertTrue(main { model.issue!!.contains("查不出来") }); assertFalse(main { model.issue!!.contains("DO_NOT_SHOW") })
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun closeCancelsRequestAndRejectsResultReturnedAfterCancellation() = runBlocking {
        val scope = scope(); val entered = CompletableDeferred<Unit>(); lateinit var continuation: Continuation<PluginReleaseCatalog.Observation>
        lateinit var work: Job
        val model = PluginInstalledVersionsModel(scope) {
            work = currentCoroutineContext()[Job]!!
            suspendCoroutine { continuation = it; entered.complete(Unit) }
        }
        try {
            main { model.refresh() }; withTimeout(3000) { entered.await() }; main { model.close() }; val cancelled = work.isCancelled
            main { continuation.resume(value()) }; idle(model); assertTrue(cancelled); assertNull(main { model.observation }); assertNull(main { model.issue })
            main { model.refresh() }; assertNull(main { model.observation })
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun unknownSelectionDoesNotBecomeKnownEmptySelection() = runBlocking {
        val scope = scope(); val model = PluginInstalledVersionsModel(scope) { value().copy(selection = null, selectionIssue = "活动选择查不出来") }
        try {
            main { model.refresh() }; idle(model)
            assertNull(main { model.observation!!.selection }); assertNotNull(main { model.observation!!.selectionIssue })
        } finally { main { model.close() }; scope.cancel() }
    }
}

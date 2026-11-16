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
class PluginUnclassifiedModelTest {
    private val ref = PluginUnclassifiedDirectories.Reference("unknown", "a".repeat(64))
    private suspend fun <T> main(block: () -> T) = withContext(Dispatchers.Main.immediate) { block() }
    private suspend fun idle(busy: () -> Boolean) = withTimeout(3000) { while (main(busy)) delay(5) }
    @Test fun unknownListingFailureAndLateResultNeverBecomeEmptyOrAnAction() = runBlocking<Unit> {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        val failed = PluginUnclassifiedModel(scope) { throw java.io.IOException("PRIVATE_DETAIL") }
        lateinit var continuation: Continuation<List<PluginUnclassifiedDirectories.Entry>>; val entered = CompletableDeferred<Unit>()
        val late = PluginUnclassifiedModel(scope) { suspendCoroutine { continuation = it; entered.complete(Unit) } }
        try {
            main { failed.refresh() }; idle { failed.busy }; assertNull(main { failed.entries }); assertNotNull(main { failed.issue }); assertFalse(main { failed.issue!!.contains("PRIVATE_DETAIL") })
            main { late.refresh() }; withTimeout(3000) { entered.await() }
            main { late.close(); continuation.resume(listOf(PluginUnclassifiedDirectories.Entry(ref.name, ref, null))) }; main { }
            assertNull(main { late.entries })
        } finally { main { failed.close(); late.close() }; scope.cancel() }
    }
    @Test fun unknownPreviewRequiresBothAcknowledgementsAndCancelResetsTheExtraOne() = exercise(false)
    @Test fun unknownSavedIntentRequiresFreshExtraAcknowledgementForExplicitResume() = exercise(true)
    private fun exercise(pending: Boolean) = runBlocking<Unit> {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); var commits = 0; var resumes = 0; val acknowledgements = mutableListOf<Boolean>()
        val view = PluginReleaseRemoval.Preview("known", ref.nameSHA256, "/owned/unknown", emptyList(), pending, unclassified = ref)
        var saved = if (pending) view else null
        val m = PluginVersionRemovalModel(scope, { saved }, { _, _, _ -> error("not version") }, { error("ordinary commit used") }, {}, { error("ordinary resume used") }, { true }, {},
            inspectUnclassifiedRequest = { _, _ -> view }, commitUnclassified = { _, ack -> acknowledgements += ack; commits++; PluginReleaseRemoval.Result(ref.nameSHA256, 1, 0, true, true, unclassified = ref) },
            resumeUnclassified = { _, ack -> acknowledgements += ack; resumes++; saved = null; PluginReleaseRemoval.Result(ref.nameSHA256, 1, 0, true, true, unclassified = ref) })
        try {
            main { m.refresh() }; idle { m.busy }
            if (!pending) { main { m.inspectUnclassified(ref) }; idle { m.busy } }
            main { m.acknowledge(true); if (pending) m.resume() else m.confirm() }
            assertEquals("extra unknown consent bypassed", 0, commits + resumes)
            if (!pending) {
                main { m.acknowledgeUnclassified(true); m.cancel(); m.inspectUnclassified(ref) }; idle { m.busy }
                assertFalse(main { m.acknowledgedUnclassified }); assertFalse(main { m.acknowledged })
            }
            main { m.acknowledge(true); m.acknowledgeUnclassified(true); if (pending) m.resume() else m.confirm() }; idle { m.busy }
            assertEquals(listOf(true), acknowledgements); assertEquals(if (pending) 0 else 1, commits); assertEquals(if (pending) 1 else 0, resumes); assertFalse(main { m.acknowledgedUnclassified })
        } finally { main { m.close() }; scope.cancel() }
    }
}

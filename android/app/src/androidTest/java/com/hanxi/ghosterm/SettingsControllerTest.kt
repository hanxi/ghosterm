package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

@RunWith(AndroidJUnit4::class)
class SettingsControllerTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String = "Settings controller did not settle", predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 5_000
        while (SystemClock.elapsedRealtime() < end) { if (main(predicate)) return; SystemClock.sleep(10) }
        fail(label)
    }
    private class WriteGate {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        fun awaitEntered() { assertTrue("Fixture: queued write did not start", entered.await(5, TimeUnit.SECONDS)) }
    }
    private class MemoryStorage : PreferencesStorage {
        @Volatile var bytes: ByteArray? = null
        @Volatile var denyRead = false
        val writes = AtomicInteger()
        val outcomes = ConcurrentLinkedQueue<PreferenceWrite>()
        val gates = ConcurrentHashMap<Int, WriteGate>()
        fun gate(number: Int) = WriteGate().also { gates[number] = it }
        override fun read(): ByteArray? {
            if (denyRead) throw IOException("injected read failure")
            return bytes?.copyOf()
        }
        override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite {
            val current = this.bytes
            if (if (expected == null) current != null else !expected.contentEquals(current)) return PreferenceWrite.Unknown
            val number = writes.incrementAndGet()
            gates[number]?.let {
                it.entered.countDown()
                check(it.release.await(10, TimeUnit.SECONDS)) { "Fixture: queued write was not released" }
            }
            val outcome = outcomes.poll() ?: PreferenceWrite.Saved
            if (outcome != PreferenceWrite.Unchanged) this.bytes = bytes.copyOf()
            return outcome
        }
        fun releaseAll() { gates.values.forEach { it.release.countDown() } }
    }
    private fun withModels(count: Int = 1, test: (MemoryStorage, AppSettings, List<SettingsViewModel>, List<ViewModelStore>) -> Unit) {
        val root = File(instrumentation.targetContext.cacheDir, "settings-controller-${UUID.randomUUID()}").apply { mkdirs() }
        val application = object : Application() { override fun getFilesDir() = root }
        val storage = MemoryStorage()
        val owner = main { AppSettings(PreferencesRepository(storage)) }
        val stores = List(count) { ViewModelStore() }
        val models = main { stores.map { store -> SettingsViewModel(application, owner).also { store.put("settings", it) } } }
        try {
            await("Initial preferences did not load") { models.all { it.canEdit } }
            test(storage, owner, models, stores)
        } finally {
            storage.releaseAll()
            main { stores.forEach(ViewModelStore::clear); owner.close() }
            runBlocking { withTimeout(5_000) { owner.awaitClosed() } }
            root.deleteRecursively()
        }
    }

    @Test fun olderReceiptsCannotClearANewerEqualCandidate() = withModels { storage, owner, models, _ ->
        val model = models.single()
        val first = storage.gate(1); val second = storage.gate(2); val third = storage.gate(3)
        main { model.edit(PreferenceEdit.FontSize(18)) }; first.awaitEntered()
        main { model.edit(PreferenceEdit.FontSize(19)); model.edit(PreferenceEdit.FontSize(18)) }
        main { assertEquals(18, model.candidate.fontSize); assertEquals(12, owner.state.value.committed.fontSize) }
        first.release.countDown(); second.awaitEntered(); await { model.inFlight == 2 }
        main { assertTrue("A newer equal edit is still pending", model.unsaved) }
        second.release.countDown(); third.awaitEntered(); await { model.inFlight == 1 }
        main {
            assertEquals(19, owner.state.value.committed.fontSize)
            assertEquals("An older commit cannot repaint the newest candidate", 18, model.candidate.fontSize)
            assertTrue(model.unsaved)
        }
        third.release.countDown(); await { model.inFlight == 0 && !model.unsaved }
        main { assertEquals(18, owner.state.value.committed.fontSize); assertEquals(18, model.candidate.fontSize) }
    }

    @Test fun failuresKeepCandidatesForRetryOrDiscard() = withModels { storage, owner, models, _ ->
        val model = models.single(); storage.outcomes.add(PreferenceWrite.Unchanged)
        main { model.edit(PreferenceEdit.FontSize(18)) }; await { model.inFlight == 0 && model.error != null }
        main { assertEquals(12, owner.state.value.committed.fontSize); assertEquals(18, model.candidate.fontSize); assertTrue(model.unsaved); model.retry() }
        await { model.inFlight == 0 && !model.unsaved }
        main { assertNull(model.error); assertEquals(18, owner.state.value.committed.fontSize) }
        storage.outcomes.add(PreferenceWrite.Unchanged)
        main { model.edit(PreferenceEdit.Blink(false)) }; await { model.inFlight == 0 && model.error != null }
        val unchanged = storage.bytes!!.copyOf()
        main { assertFalse(model.candidate.blink); model.discard(); assertTrue(model.candidate.blink); assertFalse(model.unsaved) }
        assertArrayEquals(unchanged, storage.bytes)
    }

    @Test fun windowsMergeOnlyTheirSelectedKeys() = withModels(2) { storage, owner, models, _ ->
        val first = storage.gate(1)
        main { models[0].edit(PreferenceEdit.Theme(TerminalTheme.FOREST)) }; first.awaitEntered()
        main { models[1].edit(PreferenceEdit.FontSize(18)) }; first.release.countDown()
        await { models.all { it.inFlight == 0 && !it.unsaved } }
        main {
            assertEquals(TerminalTheme.FOREST, owner.state.value.committed.terminalTheme)
            assertEquals(18, owner.state.value.committed.fontSize)
            models.forEach { assertEquals(owner.state.value.committed, it.candidate) }
        }
    }

    @Test fun unknownResultsRequireConfirmedReloadBeforeEditing() = withModels { storage, owner, models, _ ->
        val model = models.single(); storage.outcomes.add(PreferenceWrite.Unknown)
        main { model.edit(PreferenceEdit.FontSize(18)) }; await { model.inFlight == 0 && model.storage is PreferencesState.Unavailable }
        main {
            assertEquals(12, owner.state.value.committed.fontSize); assertEquals(18, model.candidate.fontSize)
            assertFalse(model.canEdit); model.requestReload(); assertTrue(model.reloadConfirmation)
            model.cancelReload(); assertTrue(model.unsaved); model.requestReload(); model.confirmReload()
        }
        await { model.inFlight == 0 && model.canEdit && !model.unsaved }
        main { assertEquals(18, model.candidate.fontSize); assertNull(model.error) }
    }

    @Test fun failedReloadRetainsTheCandidateUntilSuccessfulConfirmation() = withModels { storage, _, models, _ ->
        val model = models.single(); storage.outcomes.add(PreferenceWrite.Unchanged)
        main { model.edit(PreferenceEdit.FontSize(18)) }; await { model.inFlight == 0 && model.unsaved }
        storage.denyRead = true
        main { model.requestReload(); model.confirmReload() }; await { model.inFlight == 0 && model.storage is PreferencesState.Unavailable }
        main { assertEquals(18, model.candidate.fontSize); assertTrue(model.unsaved) }
        storage.denyRead = false
        main { model.requestReload(); model.confirmReload() }; await { model.inFlight == 0 && model.canEdit && !model.unsaved }
        main { assertEquals(12, model.candidate.fontSize) }
        assertNull(storage.bytes)
    }

    @Test fun resetRequiresConfirmationAndPublishesOneWholeTransaction() = withModels { storage, owner, models, _ ->
        val model = models.single()
        main { model.edit(PreferenceEdit.FontSize(20)); model.edit(PreferenceEdit.Theme(TerminalTheme.FOREST)) }
        await { model.inFlight == 0 && !model.unsaved }; val before = storage.writes.get()
        main { model.requestReset(); assertTrue(model.resetConfirmation); model.cancelReset() }
        assertEquals(before, storage.writes.get())
        main { assertEquals(20, model.candidate.fontSize); model.requestReset(); model.confirmReset() }
        await { model.inFlight == 0 && !model.unsaved }
        main { assertEquals(AppPreferences(), owner.state.value.committed); assertEquals(AppPreferences(), model.candidate) }
        assertEquals("Reset writes one complete document", before + 1, storage.writes.get())
    }

    @Test fun closingThePageDoesNotCancelAnAcceptedWrite() = withModels { storage, owner, models, stores ->
        val first = storage.gate(1)
        main { models.single().edit(PreferenceEdit.FontSize(18)) }; first.awaitEntered()
        main { stores.single().clear() }; first.release.countDown()
        await { owner.pendingCount.value == 0 && owner.state.value.committed.fontSize == 18 }
        assertNotNull(storage.bytes)
    }

    @Test fun leavingAnUnsavedOverviewRequiresDiscardAndConsumesTheEventOnce() = withModels { storage, _, models, _ ->
        val model = models.single(); storage.outcomes.add(PreferenceWrite.Unchanged)
        main { model.open(SettingsSection.APPEARANCE); model.edit(PreferenceEdit.FontSize(18)) }
        await { model.inFlight == 0 && model.unsaved }
        main {
            assertTrue(model.back()); assertNull(model.section); assertFalse(model.leaveConfirmation)
            assertTrue(model.back()); assertTrue(model.leaveConfirmation); model.keepEditing(); assertTrue(model.unsaved)
            model.back(); model.discardAndLeave(); assertFalse(model.unsaved)
            assertTrue(model.consumeLeave()); assertFalse(model.consumeLeave())
        }
        assertEquals(1, storage.writes.get())
    }

    @Test fun queueRejectionKeepsTheNewestDraftAfterOlderWritesFinish() = withModels { storage, owner, models, _ ->
        val model = models.single(); val first = storage.gate(1)
        main { model.edit(PreferenceEdit.FontSize(18)) }; first.awaitEntered()
        main {
            repeat(AppSettings.MAX_QUEUED) { model.edit(PreferenceEdit.FontSize(10 + it % 11)) }
            model.edit(PreferenceEdit.Theme(TerminalTheme.PAPER))
            assertNotNull(model.error)
        }
        first.release.countDown(); await { model.inFlight == 0 }
        main {
            assertTrue(model.unsaved); assertEquals(TerminalTheme.PAPER, model.candidate.terminalTheme)
            assertEquals(TerminalTheme.GRAPHITE, owner.state.value.committed.terminalTheme)
            assertNotNull(model.error); model.retry()
        }
        await { model.inFlight == 0 && !model.unsaved }
        main { assertEquals(TerminalTheme.PAPER, owner.state.value.committed.terminalTheme); assertNull(model.error) }
    }

    @Test fun aClosedOwnerRejectsNewOperationsWithoutHanging() = withModels { storage, owner, _, _ ->
        main { owner.close() }
        val result = runBlocking { withTimeout(2_000) { owner.save(UUID.randomUUID(), listOf(PreferenceEdit.FontSize(18))) } }
        assertTrue(result is PreferenceUpdate.Failed); assertEquals(0, storage.writes.get())
        assertEquals(0, owner.pendingCount.value)
    }
}

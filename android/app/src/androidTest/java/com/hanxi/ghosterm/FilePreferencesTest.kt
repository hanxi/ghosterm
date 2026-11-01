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
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/** Real local listing/publication with controlled preference write outcomes. */
@RunWith(AndroidJUnit4::class)
class FilePreferencesTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private lateinit var root: File
    private lateinit var home: File
    private lateinit var owner: AppSettings
    private lateinit var work: FileWorkRuntime
    private lateinit var browser: FilesViewModel
    private lateinit var page: FileTasksViewModel
    private val stores = mutableListOf<ViewModelStore>()
    private val reads = AtomicInteger()
    private val memory = object : PreferencesStorage {
        @Volatile var bytes: ByteArray? = null
        @Volatile var reject = false
        override fun read() = bytes?.copyOf()
        override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite {
            if (reject) return PreferenceWrite.Unchanged
            this.bytes = bytes.copyOf(); return PreferenceWrite.Saved
        }
    }
    private fun <T> main(block: () -> T): T {
        var value: T? = null
        instrumentation.runOnMainSync { value = block() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(message: String = "File preferences did not settle", predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 15_000
        while (SystemClock.elapsedRealtime() < end) { if (main(predicate)) return; SystemClock.sleep(20) }
        fail(message)
    }
    private fun edit(vararg edits: PreferenceEdit) {
        assertTrue(runBlocking { withTimeout(10_000) { owner.save(UUID.randomUUID(), edits.toList()) } } is PreferenceUpdate.Saved)
    }
    @Before fun setup() {
        val context = instrumentation.targetContext
        root = File(context.cacheDir, "file-preferences-${UUID.randomUUID()}").apply { mkdirs() }
        home = File(root, "home").apply { mkdirs() }
        listOf("source", "one", "two").forEach { File(home, it).mkdir() }
        File(home, "source/a.txt").writeText("source bytes ☃\n")
        File(home, ".hidden").writeText("hidden")
        File(home, "z.txt").writeText("large payload")
        File(home, "b.txt").writeText("b")
        val app = object : Application() { init { attachBaseContext(context) }; override fun getFilesDir() = root }
        val local = LocalFileBrowser(home, context.contentResolver)
        main {
            owner = AppSettings(PreferencesRepository(memory))
            work = FileWorkRuntime(app, owner)
            browser = FilesViewModel(app, object : FileBrowserSource by local {
                override suspend fun browse(location: FileLocation): BrowserDirectory { reads.incrementAndGet(); return local.browse(location) }
            }, work)
            page = FileTasksViewModel(app, work)
            stores += ViewModelStore().also { it.put("files", browser) }
            stores += ViewModelStore().also { it.put("tasks", page) }
            browser.enter()
        }
        await { owner.state.value.storage is PreferencesState.Ready && browser.preferencesAvailable && !browser.loading && !page.journalLoading }
    }
    @After fun cleanup() {
        if (::page.isInitialized) {
            main { page.tasks.toList().forEach(page::cancel) }
            await("Fixture: file tasks did not release before cleanup") { !page.busy }
        }
        main { if (::browser.isInitialized) browser.leave(); stores.forEach(ViewModelStore::clear); if (::owner.isInitialized) owner.close() }
        if (::owner.isInitialized) runBlocking { withTimeout(5_000) { owner.awaitClosed() } }
        if (::root.isInitialized) root.deleteRecursively()
    }
    private fun snapshot(name: String = "a.txt"): FileClipboardSnapshot {
        val source = FileLocation(FileEndpoint.APP, "source")
        val bytes = File(home, "source/$name").length().toULong()
        return FileClipboardSnapshot.capture(source, "source", listOf(BrowserEntry("source/$name", name, FileKind.FILE, bytes, null)))
    }
    private fun start(snapshot: FileClipboardSnapshot, target: String = "one"): FileTask = main {
        page.start(snapshot, FileLocation(FileEndpoint.APP, target), target) { request ->
            FileTaskConnections(request.source.endpoint, request.destination.endpoint,
                { _, _ -> error("Fixture: local copy must not resolve SSH") }, { _, _, _ -> error("Fixture: local copy must not authenticate") })
        }
        page.tasks.first()
    }
    private fun ready(task: FileTask) {
        await { task.phase == FileTaskPhase.CONFIRMING || task.phase == FileTaskPhase.FINISHED }
        assertEquals("Fixture: local task preparation: ${task.error}", FileTaskPhase.CONFIRMING, main { task.phase })
    }
    private fun confirm(task: FileTask) { ready(task); main { page.confirm(task, checkNotNull(task.decisions).reference) }; await { task.phase == FileTaskPhase.FINISHED } }

    @Test fun sizeOrderingPreservesUnsignedUnknownAndDirectorySemantics() {
        fun entry(name: String, size: ULong?, kind: FileKind = FileKind.FILE) = BrowserEntry(name, name, kind, size, null)
        val rows = listOf(entry("dir", 0u, FileKind.DIRECTORY), entry("b", null), entry("z", 0u),
            entry("A", ULong.MAX_VALUE), entry("a", ULong.MAX_VALUE), entry(".hidden", 3u))
        assertEquals(listOf("A", "a", "z", "b", "dir"), AppPreferences(sortBy = FileSort.SIZE, foldersFirst = false).orderFiles(rows).map { it.name })
        assertEquals(listOf("dir", "A", "a", "z", "b"), AppPreferences(sortBy = FileSort.SIZE).orderFiles(rows).map { it.name })
        assertEquals(listOf(".hidden", "A", "a", "b", "dir", "z"), AppPreferences(showHidden = true, foldersFirst = false).orderFiles(rows).map { it.name })
        assertEquals(6, rows.size); assertEquals(0uL, rows.first().size)
    }

    @Test fun committedFiltersAndSortingKeepTheCopiedOriginAndAvoidDirectoryReads() {
        edit(PreferenceEdit.ShowHidden(true)); await { browser.showHidden }
        val hidden = main { browser.visibleEntries.single { it.name == ".hidden" } }
        main { browser.longPress(hidden); browser.copySelected(); browser.longPress(hidden) }
        val original = main { checkNotNull(browser.clipboard) }; val count = reads.get()
        edit(PreferenceEdit.ShowHidden(false), PreferenceEdit.FoldersFirst(false), PreferenceEdit.Sort(FileSort.SIZE))
        await { !browser.showHidden }
        main {
            assertTrue("A hidden item cannot remain selected", browser.selected.isEmpty())
            assertEquals("z.txt", browser.visibleEntries.first().name)
            assertSame(original, browser.clipboard); assertEquals(listOf(hidden), original.items)
            assertFalse(browser.visibleEntries.any { it.name.startsWith(".") })
        }
        assertEquals(count, reads.get()); assertEquals("hidden", File(home, ".hidden").readText())
    }

    @Test fun failedHiddenToggleKeepsTheConfirmedFilterAndExposesTheError() {
        memory.reject = true
        main { browser.toggleHidden() }; await { !browser.hiddenSaving && browser.preferenceError != null }
        main { assertFalse(browser.showHidden); assertFalse(browser.visibleEntries.any { it.name == ".hidden" }) }
        memory.reject = false
        main { browser.toggleHidden() }; await { !browser.hiddenSaving && browser.showHidden }
        assertTrue(main { owner.state.value.committed.showHidden }); assertNull(main { browser.preferenceError })
    }

    @Test fun newRequestsCaptureConflictPolicyWithoutChangingQueuedRequests() {
        File(home, "one/a.txt").writeText("existing one"); File(home, "two/a.txt").writeText("existing two")
        edit(PreferenceEdit.Conflict(FileConflictPolicy.KEEP_BOTH)); val first = start(snapshot()); ready(first)
        edit(PreferenceEdit.Conflict(FileConflictPolicy.SKIP)); val second = start(snapshot(), "two")
        edit(PreferenceEdit.Conflict(FileConflictPolicy.ASK))
        main { assertEquals(FileConflictPolicy.KEEP_BOTH, first.request.conflictPolicy); assertEquals(FileConflictPolicy.SKIP, second.request.conflictPolicy) }
        confirm(first); confirm(second)
        assertEquals("source bytes ☃\n", File(home, "one/a (2).txt").readText())
        assertEquals(FileCopyState.ALL_SKIPPED, main { second.result?.state })
        assertEquals("existing two", File(home, "two/a.txt").readText())
    }

    @Test fun capturedClearPolicyWorksAfterTheBrowserIsDestroyed() {
        val source = snapshot(); main { work.clipboard.snapshot = source }
        edit(PreferenceEdit.Clipboard(ClipboardAfterPaste.CLEAR)); val task = start(source); ready(task)
        main { browser.leave(); stores.first().clear() }
        edit(PreferenceEdit.Clipboard(ClipboardAfterPaste.KEEP)); confirm(task)
        assertEquals(FileCopyState.COMPLETE, main { task.result?.state }); assertNull(main { work.clipboard.snapshot })
        assertEquals("source bytes ☃\n", File(home, "one/a.txt").readText())
        assertEquals("source bytes ☃\n", File(home, "source/a.txt").readText())
    }

    @Test fun capturedKeepPolicySurvivesLaterPreferenceChanges() {
        val source = snapshot(); main { work.clipboard.snapshot = source }
        val task = start(source); ready(task)
        edit(PreferenceEdit.Clipboard(ClipboardAfterPaste.CLEAR)); confirm(task)
        assertSame(source, main { work.clipboard.snapshot }); assertEquals(FileCopyState.COMPLETE, main { task.result?.state })
    }

    @Test fun completingAnOldRequestCannotClearANewerClipboard() {
        val source = snapshot(); main { work.clipboard.snapshot = source }
        edit(PreferenceEdit.Clipboard(ClipboardAfterPaste.CLEAR)); val task = start(source); ready(task)
        val replacement = snapshot(); assertNotEquals(source.id, replacement.id)
        main { work.clipboard.snapshot = replacement }; confirm(task)
        assertSame(replacement, main { work.clipboard.snapshot })
    }

    @Test fun allSkippedAndCancelledRequestsKeepTheirSourceList() {
        val source = snapshot(); main { work.clipboard.snapshot = source }
        File(home, "one/a.txt").writeText("existing")
        edit(PreferenceEdit.Conflict(FileConflictPolicy.SKIP), PreferenceEdit.Clipboard(ClipboardAfterPaste.CLEAR))
        val skipped = start(source); confirm(skipped)
        assertEquals(FileCopyState.ALL_SKIPPED, main { skipped.result?.state }); assertSame(source, main { work.clipboard.snapshot })
        val cancelled = start(source, "two"); ready(cancelled); main { page.cancel(cancelled) }
        await { cancelled.phase == FileTaskPhase.FINISHED }
        assertSame(source, main { work.clipboard.snapshot }); assertFalse(File(home, "two/a.txt").exists())
    }
}

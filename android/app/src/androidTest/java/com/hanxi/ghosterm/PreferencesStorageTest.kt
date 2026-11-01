package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.nio.file.Path
import java.nio.file.Files
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.coroutines.CoroutineContext

@RunWith(AndroidJUnit4::class)
class PreferencesStorageTest {
    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val root = File(context.filesDir, "preference-tests-${UUID.randomUUID()}").apply { mkdir() }
    private val directory = File(root, "preferences")
    private val target get() = File(directory, "preferences.json")
    @After fun cleanup() { root.deleteRecursively() }
    private fun repository(ops: ConfigFileOps = ConfigFileOps()) = PreferencesRepository(PreferencesFile(directory, ops))
    private fun ready(repository: PreferencesRepository): PreferencesState.Ready {
        val state = repository.state.value
        assertTrue("Preferences must have a committed readable snapshot", state is PreferencesState.Ready)
        return state as PreferencesState.Ready
    }
    private fun failed(update: PreferenceUpdate): PreferenceUpdate.Failed {
        assertTrue("This update must report failure", update is PreferenceUpdate.Failed)
        return update as PreferenceUpdate.Failed
    }
    private fun changed() = AppPreferences(appearance = AppAppearance.DARK, terminalTheme = TerminalTheme.FOREST,
        fontSize = 20, lineHeight = TerminalLineHeight.COMPACT, cursor = TerminalCursor.UNDERLINE,
        blink = false, auxKeys = false, keySet = AuxiliaryKeySet.EXTENDED, ctrlMode = CtrlBehavior.LOCKED,
        autoKeyboard = true, keepAwake = true, showHidden = true, sortBy = FileSort.SIZE,
        foldersFirst = false, conflict = FileConflictPolicy.KEEP_BOTH, clipboardAfterPaste = ClipboardAfterPaste.CLEAR)

    @Test fun firstLoadDoesNotCreateAFileAndAllValuesSurviveRestart() = runBlocking {
        val repository = repository()
        assertTrue(repository.reload() is PreferencesState.Ready)
        assertEquals(AppPreferences(), ready(repository).values)
        assertFalse("First read must not create an empty document", directory.exists())
        assertTrue(repository.update { changed() } is PreferenceUpdate.Saved)
        assertEquals("The owner observes its committed values", changed(), ready(repository).values)
        val next = repository()
        assertTrue(next.reload() is PreferencesState.Ready)
        assertEquals("Every committed preference survives a new owner", changed(), ready(next).values)
        assertTrue(ready(next).issues.isEmpty())
        assertEquals(0x180, Os.stat(target.path).st_mode and 0x1ff)
        assertEquals(0x1c0, Os.stat(directory.path).st_mode and 0x1ff)
    }

    @Test fun invalidIndividualValuesFallBackWithoutRewritingTheDocument() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val json = JSONObject(target.readText())
        json.getJSONObject("values").put("terminalTheme", "future-theme").put("fontSize", 21).put("blink", "true")
        val original = json.toString().toByteArray(); target.writeBytes(original)
        assertTrue(repository.reload() is PreferencesState.Ready)
        val snapshot = ready(repository)
        assertEquals(changed().copy(terminalTheme = TerminalTheme.GRAPHITE, fontSize = 12, blink = true), snapshot.values)
        assertEquals(setOf(PreferenceKey.TERMINAL_THEME, PreferenceKey.FONT_SIZE, PreferenceKey.BLINK), snapshot.issues.map { it.key }.toSet())
        assertTrue(snapshot.issues.all { it.kind == PreferenceIssueKind.INVALID })
        assertArrayEquals("Fallback does not repair the file without user intent", original, target.readBytes())
        assertTrue(repository.update { it.copy(keepAwake = false) } is PreferenceUpdate.Saved)
        assertTrue(ready(repository).issues.isEmpty())
        assertFalse(ready(repository).values.keepAwake)
    }

    @Test fun finalPrototypeChoiceIdsLoadAndSaveWithoutFallback() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val json = JSONObject(target.readText())
        json.getJSONObject("values").put("lineHeight", "relaxed").put("ctrlMode", "locked").put("conflict", "rename")
        target.writeText(json.toString())
        assertTrue(repository.reload() is PreferencesState.Ready)
        assertTrue(ready(repository).issues.isEmpty())
        assertEquals(TerminalLineHeight.RELAXED, ready(repository).values.lineHeight)
        assertEquals(CtrlBehavior.LOCKED, ready(repository).values.ctrlMode)
        assertEquals(FileConflictPolicy.KEEP_BOTH, ready(repository).values.conflict)
        assertTrue(repository.update { it.copy(fontSize = 11) } is PreferenceUpdate.Saved)
        val saved = JSONObject(target.readText()).getJSONObject("values")
        assertEquals("relaxed", saved.get("lineHeight"))
        assertEquals("locked", saved.get("ctrlMode"))
        assertEquals("rename", saved.get("conflict"))
    }

    @Test fun firstUseDefaultsMatchTheFinalPrototypeDocument() = runBlocking {
        // Independent fixture from android-prototype.html, not AppPreferences defaults.
        val expected = JSONObject("""{"appearance":"system","terminalTheme":"graphite","fontSize":12,"lineHeight":"standard","cursor":"block","blink":true,"auxKeys":true,"keySet":"basic","ctrlMode":"once","autoKeyboard":false,"keepAwake":false,"showHidden":false,"sortBy":"name","foldersFirst":true,"conflict":"ask","clipboardAfterPaste":"keep"}""")
        val repository = repository(); repository.reload()
        assertTrue(repository.update { it } is PreferenceUpdate.Saved)
        val actual = JSONObject(target.readText()).getJSONObject("values")
        assertEquals(expected.keys().asSequence().toSet(), actual.keys().asSequence().toSet())
        expected.keys().asSequence().forEach { key -> assertEquals("Prototype default: $key", expected.get(key), actual.get(key)) }
    }

    @Test fun damagedOrFutureDocumentsNeverBecomeEmptyDefaultsOnDisk() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val valid = target.readBytes()
        val future = JSONObject(valid.toString(Charsets.UTF_8)).put("version", 2).toString().toByteArray()
        // Each guard has a valid surrounding document; another parser rejection
        // must not hide a missing UTF-8, depth or size check.
        val malformedUtf8 = "{\"version\":1,\"values\":{\"appearance\":\"".toByteArray() +
            byteArrayOf(0xc3.toByte(), 0x28) + "\"}}".toByteArray()
        val deep = ("{\"version\":1,\"values\":{\"fontSize\":" + "{\"a\":".repeat(20) + "0" + "}".repeat(20) + "}}").toByteArray()
        val large = JSONObject(valid.toString(Charsets.UTF_8)).apply {
            getJSONObject("values").put("appearance", "x".repeat(8193))
        }.toString().toByteArray()
        val samples = listOf("{".toByteArray(), malformedUtf8, future, deep, large)
        for (original in samples) {
            target.writeBytes(original)
            assertTrue(repository.reload() is PreferencesState.Unavailable)
            val failed = failed(repository.update { AppPreferences() })
            assertTrue(failed.needsReload)
            assertArrayEquals("Unreadable original must not be rebuilt", original, target.readBytes())
        }
        target.writeBytes(valid)
        assertTrue(repository.reload() is PreferencesState.Ready)
        assertEquals(changed(), ready(repository).values)
    }

    @Test fun missingFieldsMigrateInMemoryAndDuplicateFieldsAreRejected() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val json = JSONObject(target.readText()); json.getJSONObject("values").remove("keepAwake")
        val old = json.toString().toByteArray(); target.writeBytes(old)
        assertTrue(repository.reload() is PreferencesState.Ready)
        assertEquals(changed().copy(keepAwake = false), ready(repository).values)
        assertEquals(setOf(PreferenceIssue(PreferenceKey.KEEP_AWAKE, PreferenceIssueKind.MISSING)), ready(repository).issues)
        assertArrayEquals(old, target.readBytes())
        val duplicate = target.readText().replace("\"fontSize\":20", "\"fontSize\":12,\"fontSize\":20").toByteArray()
        assertFalse(old.contentEquals(duplicate))
        target.writeBytes(duplicate)
        assertTrue(repository.reload() is PreferencesState.Unavailable)
        assertArrayEquals(duplicate, target.readBytes())
    }

    @Test fun prePublicationFailurePreservesCommittedValuesAndAllowsRetry() = runBlocking {
        val initial = repository(); initial.reload(); initial.update { changed() }
        val original = target.readBytes()
        var reject = true
        val repository = repository(object : ConfigFileOps() {
            override fun replace(from: Path, to: Path) {
                if (reject) throw IOException("injected before publication")
                super.replace(from, to)
            }
        })
        repository.reload()
        assertTrue(repository.update { AppPreferences() } is PreferenceUpdate.Failed)
        assertEquals(changed(), ready(repository).values)
        assertArrayEquals(original, target.readBytes())
        assertFalse(directory.listFiles()!!.any { it.name.endsWith(".tmp") })
        reject = false
        assertTrue(repository.update { AppPreferences() } is PreferenceUpdate.Saved)
        assertEquals(AppPreferences(), ready(repository).values)
    }

    @Test fun postPublicationFailureIsUnknownUntilExplicitReload() = runBlocking {
        val initial = repository(); initial.reload(); initial.update { changed() }
        val repository = repository(object : ConfigFileOps() {
            override fun syncDirectory(directory: Path) { throw IOException("injected after publication") }
        })
        repository.reload()
        val failed = failed(repository.update { AppPreferences() })
        assertTrue(failed.needsReload)
        assertTrue("Unknown publication must block writes", repository.state.value is PreferencesState.Unavailable)
        val unavailable = repository.state.value as PreferencesState.Unavailable
        assertEquals(changed(), unavailable.previous)
        val actual = target.readBytes()
        assertTrue(repository.reset() is PreferenceUpdate.Failed)
        assertArrayEquals("Unknown outcome must block more writes", actual, target.readBytes())
        assertTrue(repository.reload() is PreferencesState.Ready)
        assertEquals("Reload observes the actual published document", AppPreferences(), ready(repository).values)
    }

    @Test fun externalChangesAreNotOverwrittenAndResetOnlyTouchesPreferences() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val foreign = JSONObject(target.readText()); foreign.getJSONObject("values").put("fontSize", 11)
        val actual = foreign.toString().toByteArray(); target.writeBytes(actual)
        assertTrue(failed(repository.update { AppPreferences() }).needsReload)
        assertArrayEquals(actual, target.readBytes())
        repository.reload(); assertEquals(11, ready(repository).values.fontSize)
        val preserved = listOf("config", "known_hosts", "identity.cipher", "source.txt").associateWith {
            "preserve-$it\n".toByteArray().also { bytes -> File(root, it).writeBytes(bytes) }
        }
        assertTrue(repository.reset() is PreferenceUpdate.Saved)
        assertEquals(AppPreferences(), ready(repository).values)
        preserved.forEach { (name, bytes) -> assertArrayEquals(bytes, File(root, name).readBytes()) }
    }

    @Test fun publicationMustReadBackTheActualCandidateBeforeReportingSaved() = runBlocking {
        val initial = repository(); initial.reload(); initial.update { changed() }
        val repository = repository(object : ConfigFileOps() {
            override fun replace(from: Path, to: Path) {
                super.replace(from, to)
                // A successful rename return alone cannot prove the expected bytes.
                to.toFile().writeText("not-the-candidate")
            }
        })
        repository.reload()
        val failed = failed(repository.reset())
        assertTrue(failed.needsReload)
        assertTrue(repository.state.value is PreferencesState.Unavailable)
        assertEquals("not-the-candidate", target.readText())
    }

    @Test fun aSymbolicLinkDocumentCannotBeReadOrReplaced() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val original = target.readBytes()
        val outside = File(root, "outside.json").apply { writeBytes(original) }
        assertTrue(target.delete())
        Files.createSymbolicLink(target.toPath(), outside.toPath())
        assertTrue(repository.reload() is PreferencesState.Unavailable)
        assertTrue(failed(repository.reset()).needsReload)
        assertEquals(PreferenceWrite.Unknown, PreferencesFile(directory).write(null, original))
        assertTrue(Files.isSymbolicLink(target.toPath()))
        assertArrayEquals(original, outside.readBytes())
    }

    @Test fun aSymbolicLinkDirectoryCannotRedirectStorage() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val original = target.readBytes()
        val outside = File(root, "outside-preferences")
        Files.move(directory.toPath(), outside.toPath())
        Files.createSymbolicLink(directory.toPath(), outside.toPath())
        assertTrue(repository.reload() is PreferencesState.Unavailable)
        assertEquals(PreferenceWrite.Unknown, PreferencesFile(directory).write(null, original))
        assertTrue(Files.isSymbolicLink(directory.toPath()))
        assertArrayEquals(original, File(outside, "preferences.json").readBytes())
    }

    @Test fun accessDeniedIsNotAnAbsentFileAndDoesNotEnableReset() = runBlocking {
        val repository = repository(); repository.reload(); repository.update { changed() }
        val original = target.readBytes()
        Os.chmod(target.path, 0)
        try {
            assertTrue("Denied read is unavailable, not a fresh install", repository.reload() is PreferencesState.Unavailable)
            assertTrue(failed(repository.reset()).needsReload)
        } finally { Os.chmod(target.path, 0x180) }
        assertArrayEquals(original, target.readBytes())
    }

    @Test fun outOfRangeEditsFailBeforeAnyStorageWrite() = runBlocking {
        var writes = 0
        val repository = PreferencesRepository(object : PreferencesStorage {
            override fun read(): ByteArray? = null
            override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite {
                writes++; return PreferenceWrite.Saved
            }
        })
        repository.reload()
        for (size in listOf(9, 21, Int.MIN_VALUE, Int.MAX_VALUE)) {
            failed(repository.update { it.copy(fontSize = size) })
            assertEquals(0, writes)
            assertEquals(AppPreferences(), ready(repository).values)
        }
    }

    @Test fun rapidEditsSerializeAgainstTheLatestCommittedSnapshot() = runBlocking {
        val entered = CountDownLatch(1); val release = CountDownLatch(1); val secondIoFinished = CountDownLatch(1)
        val executor = Executors.newFixedThreadPool(2)
        val turns = AtomicInteger()
        val dispatcher = object : CoroutineDispatcher() {
            override fun dispatch(context: CoroutineContext, block: Runnable) {
                val turn = turns.incrementAndGet()
                executor.execute {
                    try { block.run() } finally {
                        // Reload is turn 1, the blocked write is turn 2. Turn 3
                        // must actually run: with the mutex it suspends; without
                        // it the second write completes before this signal.
                        if (turn == 3) secondIoFinished.countDown()
                    }
                }
            }
        }
        var stored: ByteArray? = null
        var writes = 0
        val storage = object : PreferencesStorage {
            override fun read(): ByteArray? = stored?.copyOf()
            override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite {
                assertTrue(if (expected == null) stored == null else expected.contentEquals(stored))
                writes++
                if (writes == 1) {
                    entered.countDown()
                    check(release.await(10, TimeUnit.SECONDS)) { "Fixture: blocked write was not released" }
                }
                stored = bytes.copyOf(); return PreferenceWrite.Saved
            }
        }
        val repository = PreferencesRepository(storage, dispatcher); repository.reload()
        val first = async(Dispatchers.Default) { repository.update { it.copy(fontSize = 18) } }
        try {
            assertTrue("Fixture: first write did not start", entered.await(5, TimeUnit.SECONDS))
            val second = async(Dispatchers.Default) {
                repository.update { it.copy(blink = false) }
            }
            assertTrue("Fixture: second IO turn did not complete", secondIoFinished.await(5, TimeUnit.SECONDS))
            assertEquals("An in-flight candidate is not the committed default", AppPreferences(), ready(repository).values)
            release.countDown()
            assertTrue(first.await() is PreferenceUpdate.Saved)
            assertTrue(second.await() is PreferenceUpdate.Saved)
            assertEquals(AppPreferences(fontSize = 18, blink = false), ready(repository).values)
            assertEquals(2, writes)
        } finally {
            release.countDown(); first.join()
            executor.shutdown()
            assertTrue("Fixture: executor did not terminate", executor.awaitTermination(5, TimeUnit.SECONDS))
        }
    }
}

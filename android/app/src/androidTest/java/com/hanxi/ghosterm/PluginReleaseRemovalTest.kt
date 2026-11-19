package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException

/** One-time app-owned fixture trees only, not user package directories. */
@RunWith(AndroidJUnit4::class)
class PluginReleaseRemovalTest {
    private val plugin = "org.ghosterm.fixture"
    private fun owner(root: File) = checkNotNull(PluginReleaseCatalog.open(root))
    private fun installed(root: File): String {
        val raw = JSONObject().put("pluginId", plugin).put("pluginVersion", "1.0.0").put("publisher", "unverified fixture").put("packageSHA256", "a".repeat(64)).toString().toByteArray()
        val id = PluginReleaseCatalog.sha(raw)
        PluginReleaseStage.create(root, id).use { stage ->
            stage.write("payload/nested/data", "content".toByteArray()); stage.write("payload/second", "second".toByteArray())
            stage.write("release.json", raw); stage.write("release.sig", ByteArray(64)); stage.write("policy.json", "{}".toByteArray()); stage.publish()
        }
        return id
    }
    private fun prepare(root: File, id: String, checkpoint: (String) -> Unit = {}, sync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }) =
        owner(root).use { PluginReleaseRemoval.prepare(it, id, it.selection().revision, checkpoint, sync) }
    private fun journal(root: File) = File(root, PluginReleaseCatalog.REMOVAL)
    private fun pending(root: File) = owner(root).use { checkNotNull(PluginReleaseRemoval.pending(it)) }
    private fun stopped(root: File, id: String, phase: String): PluginReleaseRemoval.Result = prepare(root, id, { if (it == phase) throw IOException("controlled checkpoint") }).use { removal -> owner(root).use { removal.confirm(it) } }
    private fun rejected(body: () -> Unit) { assertTrue("operation unexpectedly succeeded", runCatching(body).exceptionOrNull() is Exception) }
    private fun replaceRecord(root: File, bytes: ByteArray) { val path = journal(root); Os.chmod(path.path, 384); path.writeBytes(bytes); Os.chmod(path.path, 256) }
    @Test fun previewHasFullInventoryAndCancellingOnlyReleasesTargetLock() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); val initial = root.list()!!.toSet()
            prepare(root, id).use { preview ->
                val value = preview.preview("caller")
                assertFalse(value.pending); assertEquals(id, value.releaseID); assertTrue(value.members.any { it.path == "payload/nested/data" && it.sha256 == PluginReleaseCatalog.sha("content".toByteArray()) })
                assertEquals(File(root, id).canonicalPath, File(value.directory).canonicalPath); assertEquals(initial, root.list()!!.toSet())
                // The user preview must not retain a root EX flock.
                owner(root).use { assertEquals(null, it.selection().revision) }
                LocalPublicationFile.directory(root, id).use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.pin(it.fd)) }
            }
            LocalPublicationFile.directory(root, id).use { assertEquals(0, PluginInstallFiles.pin(it.fd)) }
            assertEquals(initial, root.list()!!.toSet()); assertFalse(journal(root).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun activeSelectionAndSharedRuntimeReferencesPreventPreparation() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root)
            owner(root).use { val active = it.change(plugin, id, true, null); rejected { PluginReleaseRemoval.prepare(it, id, active.selection.revision).close() }; it.change(plugin, id, false, active.selection.revision) }
            owner(root).use { catalog -> catalog.openVersion(id).use { version -> version.pin(); rejected { PluginReleaseRemoval.prepare(catalog, id, catalog.selection().revision).close() } } }
            assertTrue(File(root, id).isDirectory); assertFalse(journal(root).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun confirmedDeletionKeepsDisabledSelectionAndUnrelatedUnknownFiles() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); val unknown = File(root, ".prepare-unknown").apply { mkdir(); File(this, "preserve").writeText("unowned") }
            val choice = owner(root).use { it.change(plugin, id, false, null).selection }
            prepare(root, id).use { preview -> owner(root).use { catalog ->
                val result = preview.confirm(catalog)
                assertTrue(result.toString(), result.complete); assertTrue(result.durabilityConfirmed); assertEquals(preview.members.size + 1, result.removed); assertEquals(0, result.observedAbsent)
                assertEquals(choice, catalog.selection()); assertNull(PluginReleaseRemoval.pending(catalog))
            } }
            assertFalse(File(root, id).exists()); assertEquals("unowned", File(unknown, "preserve").readText()); assertFalse(journal(root).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun selectionRevisionChangesAndPreIntentCancellationDoNotWrite() {
        for (changed in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = installed(root)
                prepare(root, id, { if (!changed && it == "before-intent") throw IOException("cancel before effects") }).use { preview ->
                    owner(root).use { catalog ->
                        if (changed) catalog.change(plugin, id, false, null)
                        rejected { preview.confirm(catalog) }
                    }
                }
                assertTrue(File(root, id).isDirectory); assertFalse(journal(root).exists()); assertTrue(root.list()!!.none { it.startsWith(".removal-record-") })
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun changedFilesExtraMembersLinksAndReplacedDirectoriesStayUntouched() {
        for (kind in listOf("file", "extra", "symlink", "directory")) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = installed(root); val payload = File(root, "$id/payload"); val file = File(payload, "second")
                prepare(root, id).use { preview ->
                    Os.chmod(payload.path, 448)
                    when (kind) {
                        "file" -> { Os.chmod(file.path, 384); file.writeText("changed"); Os.chmod(file.path, 256) }
                        "extra" -> { File(payload, "extra").apply { writeText("preserve"); Os.chmod(path, 256) } }
                        "symlink" -> Os.symlink("second", File(payload, "extra").path)
                        "directory" -> { check(File(payload, "nested").renameTo(File(payload, "old"))); File(payload, "nested").apply { check(mkdir()); Os.chmod(path, 320) } }
                    }
                    Os.chmod(payload.path, 320)
                    owner(root).use { rejected { preview.confirm(it) } }
                }
                assertTrue(File(root, id).isDirectory); assertFalse(journal(root).exists()); assertTrue(file.isFile)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun durableIntentPrecedesDetachAndRefreshOrRestartNeverReplaysIt() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); val result = stopped(root, id, "after-intent")
            assertFalse(result.complete); assertEquals(0, result.removed); assertTrue(File(root, id).isDirectory)
            val raw = journal(root).readBytes(); val first = pending(root)
            assertTrue(first.pending); assertEquals(id, first.releaseID)
            repeat(2) { owner(root).use { catalog -> catalog.observe(); assertEquals(first, PluginReleaseRemoval.pending(catalog)) }; assertArrayEquals(raw, journal(root).readBytes()) }
            assertTrue(File(root, id).isDirectory)
            owner(root).use { assertTrue(PluginReleaseRemoval.cancelPending(it, first.id)) }
            assertFalse(journal(root).exists()); assertTrue(File(root, id).isDirectory)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun explicitResumeAfterPartialDeletionCountsNewEffectsSeparately() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); val count = prepare(root, id).use { it.members.size + 1 }
            val partial = stopped(root, id, "after-unlink")
            assertFalse(partial.complete); assertEquals(1, partial.removed); assertEquals(0, partial.observedAbsent); assertFalse(File(root, id).exists())
            val intent = pending(root)
            owner(root).use { catalog ->
                rejected { PluginReleaseRemoval.cancelPending(catalog, intent.id) }
                val result = PluginReleaseRemoval.resume(catalog, intent.id)
                assertTrue(result.toString(), result.complete); assertEquals(1, result.observedAbsent); assertEquals(count - 1, result.removed)
            }
            assertFalse(journal(root).exists()); assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun newObjectsInsideDetachedTreePreventAnyFurtherRemoval() {
        for (replace in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = installed(root); assertFalse(stopped(root, id, "after-detach").complete)
                val intent = pending(root); val target = File(intent.directory)
                Os.chmod(target.path, 448)
                if (replace) {
                    check(target.renameTo(File(root, "preserved-original"))); check(target.mkdir())
                }
                File(target, "unknown").apply { writeText("preserve"); Os.chmod(path, 256) }
                Os.chmod(target.path, 320)
                val result = runCatching { owner(root).use { PluginReleaseRemoval.resume(it, intent.id) } }
                assertTrue(result.exceptionOrNull() is Exception || result.getOrThrow().let { !it.complete && it.removed == 0 })
                assertEquals("preserve", File(target, "unknown").readText()); assertTrue(journal(root).isFile)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun pendingIntentBlocksInstallAndEnablingButAllowsExplicitDisable() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); assertFalse(stopped(root, id, "after-intent").complete)
            owner(root).use { catalog ->
                rejected { catalog.change(plugin, id, true, null) }
                val choice = catalog.change(plugin, id, false, null); assertFalse(choice.selection.choices.getValue(plugin).enabled)
            }
            rejected { PluginReleaseStage.create(root, "c".repeat(64)).close() }
            assertTrue(File(root, id).isDirectory); assertTrue(journal(root).isFile)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun corruptRecordsAreUnknownAndCannotAuthorizeRemoval() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); assertFalse(stopped(root, id, "after-intent").complete)
            val raw = journal(root).readBytes(); val text = raw.toString(Charsets.UTF_8)
            val first = JSONObject(text.lineSequence().first())
            val invalid = listOf(
                text.replaceFirst("\"schema\":1", "\"schema\":1,\"schema\":1").toByteArray(),
                text.replaceFirst("\"schema\":1,", "").toByteArray(),
                text.replaceFirst("\"schema\":1", "\"schema\":null").toByteArray(),
                text.replaceFirst("\"schema\":1", "\"schema\":\"1\"").toByteArray(),
                text.replaceFirst("\"schema\":1", "\"schema\":1.0").toByteArray(),
                text.replaceFirst("\"schema\":1", "\"schema\":4").toByteArray(),
                text.replaceFirst("\"born\":\"", "\"born\":\"0").toByteArray(),
                (first.put("extra", true).toString() + "\n" + text.substringAfter('\n')).toByteArray(),
                raw.copyOf(raw.size - 1), byteArrayOf(0xc3.toByte(), 0x28, 10),
            )
            for (bytes in invalid) {
                replaceRecord(root, bytes)
                rejected { owner(root).use { PluginReleaseRemoval.pending(it) } }
                rejected { owner(root).use { PluginReleaseRemoval.resume(it, "not-authorized") } }
                assertTrue(File(root, id).isDirectory); assertArrayEquals(bytes, journal(root).readBytes())
            }
            replaceRecord(root, raw); assertTrue(pending(root).pending)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun changedPersistentIdentityInSavedIntentCannotAuthorizeAnyUnlink() {
        for (field in listOf("root", "version")) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = installed(root); assertFalse(stopped(root, id, "after-intent").complete)
                val pending = pending(root); val raw = journal(root).readText()
                val header = JSONObject(raw.substringBefore('\n'))
                val stamp = header.getJSONObject(field); val value = stamp.getString("born").toLong()
                stamp.put("born", (if (value == 4294967295L) 1 else value + 1).toString())
                replaceRecord(root, (header.toString() + "\n" + raw.substringAfter('\n')).toByteArray())
                val result = runCatching { owner(root).use { PluginReleaseRemoval.resume(it, pending.id) } }
                assertTrue("changed persistent identity authorized deletion", result.exceptionOrNull() is Exception || result.getOrThrow().let { !it.complete && it.removed == 0 })
                assertEquals("content", File(root, "$id/payload/nested/data").readText()); assertTrue(journal(root).isFile)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun fullyAbsentTreeCanClearIntentAfterAnUnrelatedExplicitDisableChangesRevision() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root); val count = prepare(root, id).use { it.members.size + 1 }
            val interrupted = stopped(root, id, "before-clear")
            assertFalse(interrupted.complete); assertEquals(count, interrupted.removed)
            assertFalse(File(root, id).exists()); val saved = pending(root)
            val disabled = owner(root).use { it.change("org.ghosterm.other", "b".repeat(64), false, it.selection().revision).selection }
            val resumed = runCatching { owner(root).use { PluginReleaseRemoval.resume(it, saved.id) } }
            assertTrue("absent tree receipt was stuck after unrelated disable: ${resumed.exceptionOrNull()}", resumed.isSuccess)
            val result = resumed.getOrThrow()
            assertTrue("absent tree receipt was not completed: $result", result.complete)
            assertEquals(0, result.removed); assertEquals(count, result.observedAbsent)
            assertFalse(journal(root).exists()); owner(root).use { assertEquals(disabled, it.selection()) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun memberSyncFailureCountsAlreadyUnlinkedFilesAndDirectoriesBeforeReturning() {
        for (failAt in listOf(1, 2)) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = installed(root); val rootInode = Os.lstat(root.path).st_ino; var childSyncs = 0
                val interrupted = prepare(root, id, sync = { fd ->
                    if (Os.fstat(fd).st_ino != rootInode && ++childSyncs == failAt) throw IOException("controlled member sync")
                    Os.fsync(fd)
                }).use { removal -> owner(root).use { removal.confirm(it) } }
                assertFalse(interrupted.complete); assertFalse(interrupted.durabilityConfirmed)
                assertEquals("already unlinked member was omitted from effect count", failAt, interrupted.removed)
                val saved = pending(root); assertFalse(File(saved.directory, "payload/nested/data").exists())
                if (failAt == 2) assertFalse(File(saved.directory, "payload/nested").exists())
                owner(root).use {
                    val resumed = PluginReleaseRemoval.resume(it, saved.id)
                    assertTrue(resumed.toString(), resumed.complete); assertEquals(failAt, resumed.observedAbsent)
                }
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun parentSyncFailurePreservesOriginalAndIntentForExplicitRetry() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = installed(root)
            prepare(root, id, sync = { throw IOException("controlled root sync") }).use { preview -> owner(root).use { catalog ->
                val result = preview.confirm(catalog); assertFalse(result.complete); assertFalse(result.durabilityConfirmed); assertEquals(0, result.removed)
            } }
            assertTrue(File(root, id).isDirectory)
            val intent = pending(root); owner(root).use { assertTrue(PluginReleaseRemoval.resume(it, intent.id).complete) }
            assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun renamedRootAndReplacedIntentNeverAuthorizeDeletingOldContents() {
        for (changeRoot in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val moved = File(root.parentFile, root.name + "-moved")
            try {
                val id = installed(root)
                prepare(root, id, { phase -> if (phase == "after-intent") {
                    if (changeRoot) { check(root.renameTo(moved)); check(root.mkdir()); Os.chmod(root.path, 448) }
                    else { val raw = journal(root).readText(); replaceRecord(root, raw.replaceFirst("\"schema\":1", "\"schema\":2").toByteArray()) }
                } }).use { preview -> owner(root).use { catalog -> val result = preview.confirm(catalog); assertFalse(result.complete); assertEquals(0, result.removed) } }
                assertTrue(File(if (changeRoot) moved else root, id).isDirectory)
            } finally { PluginInstallTestFiles.clean(root); PluginInstallTestFiles.clean(moved) }
        }
    }
}

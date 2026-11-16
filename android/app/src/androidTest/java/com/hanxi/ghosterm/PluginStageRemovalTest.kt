package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException

@RunWith(AndroidJUnit4::class)
class PluginStageRemovalTest {
    private val f = PluginJournalFixture
    private fun prepare(root: File, ref: PluginInstallJournal.Reference, checkpoint: (String) -> Unit = {}) =
        f.owner(root).use { PluginReleaseRemoval.prepareStage(it, ref, checkpoint) }
    private fun rejected(block: () -> Unit) { assertTrue("unsafe operation succeeded", runCatching(block).exceptionOrNull() is Exception) }
    @Test fun mutablePartialStageNeedsFullPreviewAndOnlyItsConfirmedTreeIsRemoved() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root); val conflict = File(root, ref.target).apply { check(mkdir()); File(this, "preserve").writeText("unrelated") }
            val selection = f.owner(root).use { it.change("org.ghosterm.other", ref.target, false, null).selection }
            prepare(root, ref).use { removal ->
                val preview = removal.preview("known"); assertEquals(ref, preview.stage); assertEquals(ref.digest, preview.releaseID)
                assertTrue(preview.members.any { it.path == "partial" && it.stamp.mode and 128 != 0 })
                assertTrue(File(root, ref.recordName).isFile); assertTrue(File(root, ref.name).isDirectory)
                f.owner(root).use { owner ->
                    val result = removal.confirm(owner); assertTrue(result.toString(), result.complete); assertEquals(ref, result.stage)
                    assertEquals(preview.members.size + 1, result.removed); assertEquals(selection, owner.selection())
                }
            }
            assertEquals("unrelated", File(conflict, "preserve").readText()); assertFalse(File(root, ref.recordName).exists()); assertFalse(File(root, ref.name).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun mutableFilesAreAllowedOnlyForAConfirmedStageNeverAnInstalledVersion() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root); prepare(root, ref).close()
            check(File(root, ref.name).renameTo(File(root, ref.target))); Os.chmod(File(root, ref.target).path, 320)
            f.owner(root).use { owner -> rejected { PluginReleaseRemoval.prepare(owner, ref.target, owner.selection().revision).close() } }
            assertTrue(File(root, "${ref.target}/partial").isFile)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancellingPreviewWritesNothingAndRecordChangesCannotAuthorizeDeletion() {
        for (change in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root); val before = root.list()!!.toSet()
                prepare(root, ref).use { removal ->
                    assertEquals(before, root.list()!!.toSet())
                    if (change) {
                        f.replace(root, ref) { it.put("target", "b".repeat(64)) }
                        f.owner(root).use { rejected { removal.confirm(it) } }
                    }
                }
                assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists()); assertTrue(File(root, "${ref.name}/partial").isFile)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun formatTwoPartialDeletionCanExplicitlyResumeAndRetireOnlyItsOwnRecord() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root); val other = f.create(root, child = false)
            val result = prepare(root, ref) { if (it == "after-unlink") throw IOException("controlled interruption") }.use { removal -> f.owner(root).use { removal.confirm(it) } }
            assertFalse(result.complete); assertEquals(1, result.removed)
            val json = JSONObject(File(root, PluginReleaseCatalog.REMOVAL).readText().substringBefore('\n'))
            assertEquals(2, json.getInt("schema")); assertEquals(ref.digest, json.getJSONObject("stage").getString("digest"))
            f.owner(root).use { owner ->
                val pending = PluginReleaseRemoval.pending(owner)!!; assertEquals(ref, pending.stage)
                rejected { PluginReleaseRemoval.cancelPending(owner, pending.id) }
                val resumed = PluginReleaseRemoval.resume(owner, pending.id)
                assertTrue(resumed.toString(), resumed.complete); assertEquals(1, resumed.observedAbsent); assertEquals(1, resumed.removed)
            }
            assertFalse(File(root, ref.recordName).exists()); assertTrue(File(root, other.recordName).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun intentBeforeDetachCanBeWithdrawnWithoutLosingOwnershipOrMutableBytes() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root)
            prepare(root, ref) { if (it == "after-intent") throw IOException("controlled interruption") }.use { removal -> f.owner(root).use { assertFalse(removal.confirm(it).complete) } }
            f.owner(root).use { owner -> val pending = PluginReleaseRemoval.pending(owner)!!; assertTrue(PluginReleaseRemoval.cancelPending(owner, pending.id)) }
            assertTrue(File(root, ref.recordName).exists()); assertEquals("unfinished public package bytes", File(root, "${ref.name}/partial").readText())
            assertEquals(PluginInstallJournal.State.ABANDONED, PluginInstallJournal.observe(root).entries.single().state)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun extraFilesAndChangedRecordInDetachedStagePreserveRemainingContent() {
        for (record in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root)
                prepare(root, ref) { if (it == "after-detach") throw IOException("controlled interruption") }.use { removal -> f.owner(root).use { assertFalse(removal.confirm(it).complete) } }
                val saved = f.owner(root).use { PluginReleaseRemoval.pending(it)!! }
                if (record) f.replace(root, ref) { it.put("target", "b".repeat(64)) }
                else File(saved.directory, "unknown").apply { writeText("preserve"); Os.chmod(path, 256) }
                val attempt = runCatching { f.owner(root).use { PluginReleaseRemoval.resume(it, saved.id) } }
                assertTrue(attempt.exceptionOrNull() is Exception || attempt.getOrThrow().let { !it.complete && it.removed == 0 })
                assertTrue(File(saved.directory, "partial").exists()); assertTrue(File(root, ref.recordName).exists())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun metadataRetirementFailureAfterTreeRemovalKeepsIntentForExplicitCompletion() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root)
            prepare(root, ref) { phase -> if (phase == "before-clear") throw IOException("controlled before metadata") }.use { removal -> f.owner(root).use { assertFalse(removal.confirm(it).complete) } }
            val saved = f.owner(root).use { PluginReleaseRemoval.pending(it)!! }
            // Simulate metadata already retired before a lost final receipt.
            assertTrue(File(root, ref.recordName).delete())
            f.owner(root).use {
                val result = PluginReleaseRemoval.resume(it, saved.id)
                assertTrue(result.toString(), result.complete); assertEquals(0, result.removed); assertEquals(2, result.observedAbsent)
            }
            assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
}

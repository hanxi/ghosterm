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
import java.util.UUID

internal object PluginUnknownFixture {
    fun directory(root: File, name: String = "旧暂存-${UUID.randomUUID()}") = File(root, name).apply {
        check(mkdir()); Os.chmod(path, 448); File(this, "partial").apply { writeText("current unclassified bytes"); Os.chmod(path, 384) }
    }
    fun reference(root: File, name: String) = PluginJournalFixture.owner(root).use { owner ->
        PluginUnclassifiedDirectories.listing(owner).single { it.name == name }.reference!!
    }
    fun prepare(root: File, ref: PluginUnclassifiedDirectories.Reference, checkpoint: (String) -> Unit = {}) =
        PluginJournalFixture.owner(root).use { PluginReleaseRemoval.prepareUnclassified(it, ref, checkpoint) }
    fun reject(message: String = "unsafe unknown deletion accepted", block: () -> Unit) { assertTrue(message, runCatching(block).exceptionOrNull() is Exception) }
}

@RunWith(AndroidJUnit4::class)
class PluginUnclassifiedDirectoriesTest {
    private val f = PluginUnknownFixture
    private fun root(block: (File) -> Unit) { val root = PluginInstallTestFiles.root(); try { block(root) } finally { PluginInstallTestFiles.clean(root) } }
    @Test fun listingExcludesKnownStagesAndMetadataButPreservesFilesAndLinks() = root { root ->
        val stage = PluginJournalFixture.create(root); val unknown = f.directory(root)
        File(root, ".selection-hidden").mkdir(); File(root, "file").writeText("preserve"); Os.symlink(unknown.path, File(root, "link").path)
        val before = root.list()!!.toSet()
        PluginJournalFixture.owner(root).use { owner ->
            val rows = PluginUnclassifiedDirectories.listing(owner)
            assertEquals(setOf(unknown.name, "file", "link"), rows.map { it.name }.toSet())
            assertNotNull(rows.single { it.name == unknown.name }.reference)
            assertTrue(rows.filter { it.name in setOf("file", "link") }.all { it.reference == null && it.issue != null })
        }
        assertEquals(before, root.list()!!.toSet()); assertTrue(File(root, stage.recordName).isFile)
    }
    @Test fun unconfirmedStageIsANewDecisionAndNeverRetiresItsOwnershipRecord() = root { root ->
        val stage = PluginJournalFixture.create(root, recorded = false); val raw = File(root, stage.recordName).readBytes()
        val ref = f.reference(root, stage.name)
        f.prepare(root, ref).use { removal ->
            assertEquals(ref, removal.preview("known").unclassified)
            PluginJournalFixture.owner(root).use { owner -> assertTrue(removal.confirm(owner, acknowledgedUnclassified = true).complete) }
        }
        assertArrayEquals(raw, File(root, stage.recordName).readBytes())
        assertEquals(PluginInstallJournal.State.ABSENT, PluginInstallJournal.observe(root).entries.single().state)
    }
    @Test fun fullMutableTreeRequiresExtraNativeAcknowledgementAndDoesNotGrantOrigin() = root { root ->
        val folder = f.directory(root)
        val nested = File(folder, "sub").apply { check(mkdir()); Os.chmod(path, 448) }
        File(nested, "child").apply { writeText("nested partial bytes"); Os.chmod(path, 384) }
        val ref = f.reference(root, folder.name)
        f.prepare(root, ref).use { removal ->
            assertEquals(ref.nameSHA256, removal.releaseID); assertEquals(3, removal.members.size)
            PluginJournalFixture.owner(root).use { owner ->
                f.reject("unknown removal bypassed extra acknowledgement") { removal.confirm(owner) }
                assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists()); assertTrue(folder.exists())
                val result = removal.confirm(owner, acknowledgedUnclassified = true)
                assertTrue(result.toString(), result.complete); assertEquals(4, result.removed); assertEquals(ref, result.unclassified)
                assertTrue(result.forgottenPackages.isEmpty()); assertTrue(owner.selection().choices.isEmpty())
            }
        }
        assertTrue(root.list()!!.isEmpty())
    }
    @Test fun observationTokenBindsParentNameAndFullDirectoryStamp() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        f.reject { f.prepare(root, ref.copy(token = "a".repeat(64))).close() }
        val renamed = File(root, "renamed"); check(folder.renameTo(renamed))
        f.reject { f.prepare(root, ref.copy(name = renamed.name)).close() }
        check(renamed.renameTo(folder)); val fresh = f.reference(root, folder.name)
        File(folder, "new").writeText("new")
        f.reject("changed directory stamp accepted") { f.prepare(root, fresh).close() }
    }
    @Test fun replacementObjectAndRootCannotReuseAnOldObservation() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        check(folder.renameTo(File(root, "preserved"))); f.directory(root, folder.name)
        f.reject { f.prepare(root, ref).close() }
        val other = PluginInstallTestFiles.root()
        try { f.directory(other, folder.name); f.reject { f.prepare(other, ref).close() } } finally { PluginInstallTestFiles.clean(other) }
    }
    @Test fun sharedReferencesPreventUnknownPreviewAndCloseReleasesExclusiveReference() = root { root ->
        val folder = f.directory(root); Os.chmod(folder.path, 320); val ref = f.reference(root, folder.name)
        LocalPublicationFile.directory(root, folder.name).use { shared ->
            assertEquals(0, PluginInstallFiles.pin(shared.fd)); f.reject { f.prepare(root, ref).close() }
        }
        f.prepare(root, ref).use {
            LocalPublicationFile.directory(root, folder.name).use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
        }
        f.prepare(root, ref).close(); assertTrue(folder.exists())
    }
    @Test fun enabledBrokenVersionCannotBypassDisableThroughUnknownEntry() = root { root ->
        val name = "c".repeat(64); val folder = f.directory(root, name)
        PluginJournalFixture.owner(root).use { it.change("org.ghosterm.broken", name, true, null) }
        val ref = f.reference(root, name)
        f.reject("enabled unknown preview accepted") { f.prepare(root, ref).close() }
        PluginJournalFixture.owner(root).use { it.change("org.ghosterm.broken", name, false, it.selection().revision) }
        f.prepare(root, ref).close(); assertTrue(folder.exists())
    }
    @Test fun corruptSelectionOrOwnershipCannotTurnIntoDeletionAuthority() = root { root ->
        f.directory(root)
        val record = File(root, PluginReleaseCatalog.SELECTION).apply { writeText("broken"); Os.chmod(path, 256) }
        f.reject { PluginJournalFixture.owner(root).use { PluginUnclassifiedDirectories.listing(it) } }
        check(record.delete()); File(root, ".install-invalid.json").apply { writeText("broken"); Os.chmod(path, 256) }
        f.reject { PluginJournalFixture.owner(root).use { PluginUnclassifiedDirectories.listing(it) } }
    }
    @Test fun reservedNamesAreRejectedByListingReferenceAndNativeMove() = root { root ->
        for (name in listOf(".", "..", "a/b", "a\\b", "a:b", ".install-x", ".intent-x", ".selection-x", ".removal-record-x", PluginReleaseCatalog.SELECTION, PluginReleaseCatalog.REMOVAL)) {
            assertFalse("reserved name accepted: $name", PluginUnclassifiedDirectories.allowedName(name))
            f.reject { f.prepare(root, PluginUnclassifiedDirectories.Reference(name, "a".repeat(64))).close() }
        }
        val folder = f.directory(root, ".selection-owned")
        LocalPublicationFile.directory(root, "").use { parent -> LocalPublicationFile.childDirectory(parent.fd, folder.name).use { child ->
            assertEquals(OsConstants.EINVAL, PluginInstallFiles.detachUnclassified(parent.fd, child.fd, folder.name, ".removing-${UUID.randomUUID()}"))
        } }
        assertTrue(folder.exists())
    }
    @Test fun linksSpecialFilesAndOversizedTreeCannotBeReviewedAsSafeContents() = root { root ->
        for (kind in listOf("symlink", "fifo", "large")) {
            val folder = f.directory(root, kind)
            when (kind) {
                "symlink" -> Os.symlink(File(folder, "partial").path, File(folder, "unsafe").path)
                "fifo" -> Os.mkfifo(File(folder, "unsafe").path, 384)
                else -> { java.io.RandomAccessFile(File(folder, "unsafe"), "rw").use { it.setLength(16777217) }; Os.chmod(File(folder, "unsafe").path, 384) }
            }
            val ref = f.reference(root, kind); f.reject { f.prepare(root, ref).close() }; assertTrue(folder.exists())
        }
    }
    @Test fun completePreviewDetectsChangedMemberEvenWhenDirectoryStampIsUnchanged() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        f.prepare(root, ref).use { removal ->
            File(folder, "partial").writeText("changed same directory")
            PluginJournalFixture.owner(root).use { owner -> f.reject { removal.confirm(owner, acknowledgedUnclassified = true) } }
        }
        assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists()); assertTrue(folder.exists())
    }
    @Test fun formatThreeResumeNeedsExtraAckAndPreservesNewOrChangedMembers() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        f.prepare(root, ref) { if (it == "after-detach") throw IOException("controlled") }.use { removal -> PluginJournalFixture.owner(root).use { assertFalse(removal.confirm(it, acknowledgedUnclassified = true).complete) } }
        PluginJournalFixture.owner(root).use { owner ->
            val saved = PluginReleaseRemoval.pending(owner)!!; assertEquals(ref, saved.unclassified)
            assertEquals(3, JSONObject(File(root, PluginReleaseCatalog.REMOVAL).readText().substringBefore('\n')).getInt("schema"))
            f.reject("resume bypassed extra acknowledgement") { PluginReleaseRemoval.resume(owner, saved.id) }
            File(saved.directory, "new").writeText("preserve")
            val result = PluginReleaseRemoval.resume(owner, saved.id, acknowledgedUnclassified = true)
            assertFalse(result.complete); assertEquals(0, result.removed); assertTrue(File(saved.directory, "partial").exists())
        }
    }
    @Test fun detachedUnknownStillHonorsNewEnabledSelectionBeforeAnyUnlink() = root { root ->
        val name = "d".repeat(64); val folder = f.directory(root, name); val ref = f.reference(root, name)
        f.prepare(root, ref) { if (it == "after-detach") throw IOException("controlled") }.use { removal -> PluginJournalFixture.owner(root).use { assertFalse(removal.confirm(it, acknowledgedUnclassified = true).complete) } }
        PluginJournalFixture.owner(root).use { owner ->
            // Adversarial stored selection, not an enable transaction permitted by the product.
            val record = JSONObject().put("schemaVersion", 1).put("nonce", UUID.randomUUID().toString()).put("choices", JSONObject().put("org.ghosterm.broken", JSONObject().put("releaseID", name).put("enabled", true).put("generation", UUID.randomUUID().toString())))
            File(root, PluginReleaseCatalog.SELECTION).apply { writeText(record.toString()); Os.chmod(path, 256) }
            val saved = PluginReleaseRemoval.pending(owner)!!
            val result = PluginReleaseRemoval.resume(owner, saved.id, acknowledgedUnclassified = true)
            assertFalse("enabled detached tree removed", result.complete); assertEquals(0, result.removed); assertTrue(File(saved.directory, "partial").exists())
        }
    }
    @Test fun partialDeletionResumesWithAccurateEffectsAndNoOwnershipRetirement() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        f.prepare(root, ref) { if (it == "after-unlink") throw IOException("controlled") }.use { removal -> PluginJournalFixture.owner(root).use {
            val result = removal.confirm(it, acknowledgedUnclassified = true); assertFalse(result.complete); assertEquals(1, result.removed)
        } }
        PluginJournalFixture.owner(root).use { owner ->
            val saved = PluginReleaseRemoval.pending(owner)!!; f.reject { PluginReleaseRemoval.cancelPending(owner, saved.id) }
            val result = PluginReleaseRemoval.resume(owner, saved.id, acknowledgedUnclassified = true)
            assertTrue(result.toString(), result.complete); assertEquals(1, result.removed); assertEquals(1, result.observedAbsent)
        }
        assertTrue(root.list()!!.isEmpty())
    }
    @Test fun beforeDetachIntentCanBeWithdrawnAndNameTamperingIsNeverFollowed() = root { root ->
        val folder = f.directory(root); val ref = f.reference(root, folder.name)
        f.prepare(root, ref) { if (it == "after-intent") throw IOException("controlled") }.use { removal -> PluginJournalFixture.owner(root).use { assertFalse(removal.confirm(it, acknowledgedUnclassified = true).complete) } }
        val record = File(root, PluginReleaseCatalog.REMOVAL); val raw = record.readBytes(); val lines = record.readText().split('\n').toMutableList()
        val header = JSONObject(lines[0]); header.getJSONObject("unclassified").put("name", "different"); lines[0] = header.toString()
        Os.chmod(record.path, 384); record.writeText(lines.joinToString("\n")); Os.chmod(record.path, 256)
        PluginJournalFixture.owner(root).use { owner -> f.reject("intent original name was not bound") { PluginReleaseRemoval.pending(owner) } }
        Os.chmod(record.path, 384); record.writeBytes(raw); Os.chmod(record.path, 256)
        PluginJournalFixture.owner(root).use { assertTrue(PluginReleaseRemoval.cancelPending(it, PluginReleaseRemoval.pending(it)!!.id)) }
        assertTrue(folder.isDirectory); assertFalse(record.exists())
    }
    @Test fun nativeUnknownMoveDoesNotOverwriteOrFollowReplacementOrConsumeBorrowedFD() = root { root ->
        val folder = f.directory(root); val target = ".removing-${UUID.randomUUID()}"
        LocalPublicationFile.directory(root, "").use { parent -> LocalPublicationFile.childDirectory(parent.fd, folder.name).use { child ->
            val conflict = File(root, target).apply { check(mkdir()) }
            assertEquals(OsConstants.EEXIST, PluginInstallFiles.detachUnclassified(parent.fd, child.fd, folder.name, target)); assertTrue(conflict.exists())
            assertTrue(conflict.delete()); check(folder.renameTo(File(root, "original"))); f.directory(root, folder.name)
            assertEquals(OsConstants.EINVAL, PluginInstallFiles.detachUnclassified(parent.fd, child.fd, folder.name, target)); assertTrue(Os.fstat(child.fileDescriptor).st_ino > 0)
            PluginInstallTestFiles.clean(File(root, folder.name)); Os.symlink(File(root, "original").path, folder.path)
            assertEquals(OsConstants.EINVAL, PluginInstallFiles.detachUnclassified(parent.fd, child.fd, folder.name, target))
            assertTrue(java.nio.file.Files.isSymbolicLink(folder.toPath()))
        } }
    }
}

package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class PluginVersionSelectionControllerTest {
    private val fixture = PluginVersionSelectionFixture
    private fun catalog(root: File) = checkNotNull(PluginReleaseCatalog.open(root))
    private suspend fun preview(owner: ExtensionController, root: File, slot: String, policy: ByteArray = fixture.policy): ExtensionController.VersionPreview {
        val revision = catalog(root).use { it.selection().revision }
        return owner.previewInstalledVersion(UUID.randomUUID().toString(), slot, revision, policy)
    }
    private suspend fun select(owner: ExtensionController, root: File, slot: String): ExtensionController.VersionSelectionResult =
        owner.confirmInstalledVersion(preview(owner, root, slot).id, true)
    private fun lock(root: File, slot: String) = LocalPublicationFile.directory(root, slot).use { PluginInstallFiles.lock(it.fd) }
    private fun replace(file: File, bytes: ByteArray) { Os.chmod(file.path, 384); file.writeBytes(bytes); Os.chmod(file.path, 256) }
    private suspend fun rejected(message: String, block: suspend () -> Unit): Throwable {
        val failure = runCatching { block() }.exceptionOrNull()
        assertTrue("$message: $failure", failure is Exception); return failure!!
    }
    @Test fun previewPinsOnlyCandidateAndRequiresSeparateAcknowledgementAndSourceApproval() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val installed = fixture.install(owner, root); val slot = installed.publication.releaseID
            val view = preview(owner, root, slot)
            assertEquals(ExtensionController.PreviousVersionState.NONE, view.previousState)
            assertEquals(installed.snapshot, view.candidate)
            assertEquals(listOf("session.rpc"), view.addedPermissions!!.map { it.capability })
            assertEquals(OsConstants.EAGAIN, lock(root, slot)); assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists())
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            rejected("missing acknowledgement selected version") { owner.confirmInstalledVersion(view.id, false) }
            assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists())
            rejected("wrong preview token selected version") { owner.confirmInstalledVersion(UUID.randomUUID().toString(), true) }
            val result = owner.confirmInstalledVersion(view.id, true); val review = result.review!!
            assertTrue(result.durabilityConfirmed); assertFalse(review.sourceApproved); assertEquals(0, lock(root, slot))
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture", review.id, review.digest).status)
            rejected("selection implicitly granted source") { owner.decodeClientArguments(review.id, review.digest, "fixture", emptyList()) }
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(review.id, review.digest))
            assertEquals(ExtensionRegistry.Status.OK, owner.resolve("fixture", review.id, review.digest).status)
            rejected("consumed preview was reusable") { owner.confirmInstalledVersion(view.id, true) }
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun previewCancelAndControllerCloseReleaseKnownTokenLease() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID
            val first = preview(owner, root, slot)
            rejected("overlapping preview replaced live owner") { preview(owner, root, slot) }
            owner.cancelVersionPreview(first.id); owner.installedVersions() // worker FIFO barrier
            assertEquals(0, lock(root, slot))
            rejected("cancelled preview selected version") { owner.confirmInstalledVersion(first.id, true) }
            preview(owner, root, slot); assertEquals(OsConstants.EAGAIN, lock(root, slot))
            owner.close(); assertEquals(0, lock(root, slot)); assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun currentPolicyRevocationIsUsedInsteadOfStoredInstallationPolicy() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID
            val policy = JSONObject(String(fixture.policy)).apply { getJSONArray("revokedKeyIds").put(getJSONArray("keys").getJSONObject(0).getString("id")) }.toString().toByteArray()
            val failure = rejected("stored policy overrode current revocation") { preview(owner, root, slot, policy) }
            assertTrue(failure is PluginReleaseSignature.Rejected && failure.reason == PluginReleaseSignature.Failure.REVOKED_KEY)
            assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists()); assertEquals(0, lock(root, slot))
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun authenticatedIdentityMustMatchActualCorePackageNotDeclaredSlotMetadata() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.identityMismatch(root)
            val failure = rejected("signed wrong Core identity was selected") { preview(owner, root, slot) }
            assertTrue(failure is PluginReleaseSignature.Rejected && failure.reason == PluginReleaseSignature.Failure.IDENTITY)
            assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists()); assertEquals(0, lock(root, slot))
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun upgradeAndRollbackUseDifferentDigestsFullPermissionDiffAndFreshSourceApprovals() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root()
        val asset = "capability-versions.json"
        val policy = fixture.policy(asset)
        val owner = fixture.controller(root, listOf("session.rpc", "ui.diff", "ui.table"))
        try {
            val a = fixture.install(owner, root, asset = asset)
            val b = fixture.install(owner, root, "2.0.0", asset)
            val first = owner.confirmInstalledVersion(preview(owner, root, a.publication.releaseID, policy).id, true)
            val old = first.review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(old.id, old.digest))
            val view = preview(owner, root, b.publication.releaseID, policy)
            assertEquals(ExtensionController.PreviousVersionState.VERIFIED, view.previousState)
            assertEquals(old.digest, view.previous!!.review.digest); assertNotEquals(old.digest, view.candidate.review.digest)
            assertEquals(2, view.addedPermissions!!.size); assertEquals(2, view.removedPermissions!!.size)
            assertEquals(listOf("fixture-worker-v2"), view.addedPermissions!!.single { it.side == "executor" }.programs)
            val next = owner.confirmInstalledVersion(view.id, true); val upgraded = next.review!!
            assertEquals(ExtensionRegistry.Status.STALE, owner.approve(old.id, old.digest))
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(upgraded.id, upgraded.digest))
            assertEquals(upgraded.digest, owner.resolve("fixture").packageDigest)
            val rollback = owner.confirmInstalledVersion(preview(owner, root, a.publication.releaseID, policy).id, true)
            assertNotEquals(first.observation.selection!!.choices[old.id]!!.generation, rollback.observation.selection!!.choices[old.id]!!.generation)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(old.id, old.digest))
            assertEquals(old.digest, owner.resolve("fixture").packageDigest)
            assertEquals(ExtensionRegistry.Status.STALE, owner.resolve("fixture", upgraded.id, upgraded.digest).status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun disableRequiresCurrentRevisionAndAckThenBlocksAllNewAdmissionUntilReselection() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID; val active = select(owner, root, slot); val review = active.review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(review.id, review.digest))
            rejected("disable bypassed acknowledgement") { owner.disableInstalledVersion(review.id, active.observation.selection!!.revision, false) }
            rejected("disable overwrote newer revision") { owner.disableInstalledVersion(review.id, null, true) }
            val disabled = owner.disableInstalledVersion(review.id, active.observation.selection!!.revision, true)
            assertFalse(disabled.observation.selection!!.choices[review.id]!!.enabled)
            assertEquals(ExtensionRegistry.Status.DENIED, owner.approve(review.id, review.digest))
            assertEquals(ExtensionRegistry.Status.DENIED, owner.resolve("fixture", review.id, review.digest).status)
            val denied = rejected("disabled version decoded arguments") { owner.decodeClientArguments(review.id, review.digest, "fixture", emptyList()) }
            assertTrue(denied is ExtensionRegistry.RegistryException && denied.status == ExtensionRegistry.Status.DENIED)
            val restored = select(owner, root, slot)
            assertNotEquals(active.observation.selection!!.choices[review.id]!!.generation, restored.observation.selection!!.choices[review.id]!!.generation)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(review.id, review.digest))
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun anotherControllerSameVersionSelectionMakesOldApprovalStaleBeforeArgumentsOrSessionEffects() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val first = fixture.controller(root); val second = fixture.controller(root)
        try {
            val slot = fixture.install(first, root).publication.releaseID; val review = select(first, root, slot).review!!
            assertEquals(ExtensionRegistry.Status.OK, first.approve(review.id, review.digest))
            select(second, root, slot)
            assertEquals(ExtensionRegistry.Status.STALE, first.resolve("fixture", review.id, review.digest).status)
            assertEquals(ExtensionRegistry.Status.STALE, first.approve(review.id, review.digest))
            val args = rejected("old approval decoded arguments") { first.decodeClientArguments(review.id, review.digest, "fixture", emptyList()) }
            assertTrue(args is ExtensionRegistry.RegistryException && args.status == ExtensionRegistry.Status.STALE)
            // No channel exists: STALE must precede channel validation/adoption.
            val session = rejected("old approval reached channel") { first.adoptSSHSession(review.id, review.digest, "a".repeat(64), "1.0.0", "fixture", emptyList()) }
            assertTrue(session is ExtensionRegistry.RegistryException && session.status == ExtensionRegistry.Status.STALE)
            select(first, root, slot); assertEquals(ExtensionRegistry.Status.UNKNOWN, first.resolve("fixture").status)
        } finally { first.close(); second.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun installedDevelopmentCopyCannotBypassMissingSelectionOrDisabledChoice() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val installed = fixture.install(owner, root); val review = owner.review(File(installed.publication.directory, "payload"))
            assertEquals(ExtensionRegistry.Status.DENIED, owner.approve(review.id, review.digest))
            assertEquals(ExtensionRegistry.Status.DENIED, owner.resolve("fixture", review.id, review.digest).status)
            val result = select(owner, root, installed.publication.releaseID)
            owner.disableInstalledVersion(review.id, result.observation.selection!!.revision, true)
            val reopened = fixture.controller(root)
            try {
                val copy = reopened.review(File(installed.publication.directory, "payload"))
                assertEquals(ExtensionRegistry.Status.DENIED, reopened.approve(copy.id, copy.digest))
            } finally { reopened.close() }
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun missingRootAndCorruptChoiceNeverRestoreUnmanagedDevelopmentAdmission() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val moved = File(root.parentFile, root.name + "-moved"); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID; val review = select(owner, root, slot).review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(review.id, review.digest))
            assertTrue(root.renameTo(moved))
            assertEquals(ExtensionRegistry.Status.STALE, owner.approve(review.id, review.digest))
            assertTrue(moved.renameTo(root))
            replace(File(root, PluginReleaseCatalog.SELECTION), "{}".toByteArray())
            val failure = rejected("corrupt choice silently granted source") { owner.approve(review.id, review.digest) }
            assertTrue(failure is PluginReleaseCatalog.Rejected && failure.reason == PluginReleaseCatalog.Reason.INVALID)
            assertNull(owner.installedVersions().selection)
        } finally { owner.close(); PluginInstallTestFiles.clean(root); PluginInstallTestFiles.clean(moved) }
    }
    @Test fun missingPreviousVersionRemainsUnknownAndDoesNotClaimEmptyPermissions() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val installed = fixture.install(owner, root); val id = installed.snapshot.review.id
            catalog(root).use { it.change(id, "f".repeat(64), true, null) }
            val view = preview(owner, root, installed.publication.releaseID)
            assertEquals(ExtensionController.PreviousVersionState.UNKNOWN, view.previousState)
            assertEquals("f".repeat(64), view.previousReleaseID); assertNull(view.previous)
            assertNull(view.addedPermissions); assertNull(view.removedPermissions)
            assertEquals(listOf("session.rpc"), view.candidate.declarations.permissions.map { it.capability })
            owner.confirmInstalledVersion(view.id, true)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun stalePreviewCannotOverwriteAnotherControllerSelection() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root); val other = fixture.controller(root)
        try {
            val a = fixture.install(owner, root); val b = fixture.install(owner, root, "2.0.0")
            val stale = preview(owner, root, a.publication.releaseID)
            val fresh = select(other, root, b.publication.releaseID)
            rejected("stale preview replaced new selection") { owner.confirmInstalledVersion(stale.id, true) }
            assertEquals(fresh.observation.selection, catalog(root).use { it.selection() }); assertEquals(0, lock(root, a.publication.releaseID))
        } finally { owner.close(); other.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun commitRechecksSignatureAndCoreBytesBeforeChangingChoice() = runBlocking<Unit> {
        for (kind in listOf("signature", "payload")) {
            val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
            try {
                val installed = fixture.install(owner, root); val view = preview(owner, root, installed.publication.releaseID)
                val target = File(installed.publication.directory, if (kind == "signature") "release.sig" else "payload/main.lua")
                val bytes = target.readBytes(); bytes[0] = (bytes[0].toInt() xor 1).toByte(); replace(target, bytes)
                rejected("changed $kind was selected") { owner.confirmInstalledVersion(view.id, true) }
                assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists()); assertEquals(0, lock(root, installed.publication.releaseID))
            } finally { owner.close(); PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun replacedRootVersionOrPayloadCannotUseAnEarlierPreviewIdentity() = runBlocking<Unit> {
        for (kind in listOf("root", "version", "payload")) {
            val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root); var moved: File? = null
            try {
                val installed = fixture.install(owner, root); val view = preview(owner, root, installed.publication.releaseID)
                val target = when (kind) { "root" -> root; "version" -> installed.publication.directory; else -> File(installed.publication.directory, "payload") }
                if (kind == "payload") Os.chmod(installed.publication.directory.path, 448)
                moved = File(target.parentFile, target.name + "-old"); assertTrue(target.renameTo(moved))
                assertTrue(target.mkdir()); Os.chmod(target.path, if (kind == "root") 448 else 320)
                if (kind == "payload") Os.chmod(installed.publication.directory.path, 320)
                rejected("replaced $kind used old preview") { owner.confirmInstalledVersion(view.id, true) }
                assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists())
            } finally { owner.close(); PluginInstallTestFiles.clean(root); moved?.let(PluginInstallTestFiles::clean) }
        }
    }
    @Test fun cancellationWhileBeforeRenameCheckpointIsBlockedLeavesOldChoiceAndNoTemporary() = runBlocking<Unit> {
        for (disabling in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root); val release = CountDownLatch(1)
            try {
                val installed = fixture.install(owner, root)
                if (disabling) select(owner, root, installed.publication.releaseID)
                val previous = catalog(root).use { it.selection() }; val names = root.list()!!.toSet()
                val view = if (disabling) null else preview(owner, root, installed.publication.releaseID)
                val entered = CompletableDeferred<Unit>()
                val checkpoint: (String) -> Unit = { phase ->
                    if (phase == "before-rename") { entered.complete(Unit); check(release.await(5, TimeUnit.SECONDS)) }
                }
                val work = launch(Dispatchers.Default) {
                    if (disabling) owner.disableInstalledVersion(installed.snapshot.review.id, previous.revision, true, checkpoint)
                    else owner.confirmInstalledVersion(view!!.id, true, checkpoint)
                }
                try {
                    withTimeout(5000) { entered.await() }; work.cancel(); release.countDown(); withTimeout(5000) { work.join() }
                    assertTrue(work.isCancelled)
                    assertEquals("cancelled confirmation changed choice; disabling=$disabling", previous, catalog(root).use { it.selection() })
                    assertEquals(names, root.list()!!.toSet()); assertEquals(0, lock(root, installed.publication.releaseID))
                } finally { release.countDown(); work.cancelAndJoin() }
            } finally { release.countDown(); owner.close(); PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun failedSyncAfterRenameReturnsPublishedUnknownDurabilityAndNeverRollsBack() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID; val view = preview(owner, root, slot)
            val selected = owner.confirmInstalledVersion(view.id, true, parentSync = { throw java.io.IOException("controlled sync") })
            assertFalse(selected.durabilityConfirmed); assertEquals(selected.observation.selection, catalog(root).use { it.selection() })
            val review = selected.review!!; assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            val disabled = owner.disableInstalledVersion(review.id, selected.observation.selection!!.revision, true, parentSync = { throw java.io.IOException("controlled sync") })
            assertFalse(disabled.durabilityConfirmed); assertEquals(disabled.observation.selection, catalog(root).use { it.selection() })
            assertEquals(ExtensionRegistry.Status.DENIED, owner.approve(review.id, review.digest))
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
}

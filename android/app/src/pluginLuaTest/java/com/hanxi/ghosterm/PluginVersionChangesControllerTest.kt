package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginVersionChangesControllerTest {
    private val f = PluginVersionSelectionFixture
    private val asset = "capability-versions.json"
    private val capabilities = listOf("session.rpc", "ui.table", "ui.diff")
    private suspend fun preview(owner: ExtensionController, root: File, slot: String) = owner.previewInstalledVersion(UUID.randomUUID().toString(), slot,
        checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().revision }, f.policy(asset))
    @Test fun actualSignedCoreFirstUpgradeAndDisabledRollbackShowBothCapabilityDirectionsAndFullDeclarationsWithoutExecution() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root, capabilities)
        try {
            val a = f.install(owner, root, "1.0.0", asset); val b = f.install(owner, root, "2.0.0", asset)
            val first = preview(owner, root, a.publication.releaseID)
            assertEquals(listOf("session.rpc", "ui.diff"), first.addedCapabilities)
            assertTrue(first.removedCapabilities!!.isEmpty()); assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            val old = owner.confirmInstalledVersion(first.id, true).review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(old.id, old.digest))
            val next = preview(owner, root, b.publication.releaseID)
            assertEquals(listOf("ui.table"), next.addedCapabilities); assertEquals(listOf("ui.diff"), next.removedCapabilities)
            assertEquals(2, next.addedPermissions!!.size); assertEquals(2, next.removedPermissions!!.size)
            assertEquals(listOf("fixture-worker-v2"), next.addedPermissions!!.single { it.side == "executor" }.programs)
            assertTrue(next.candidate.review.optionalMissingCapabilities.isEmpty())
            assertTrue(next.candidate.declarations.executorJSON!!.contains("executor.lua"))
            assertEquals("^1.0.0", next.candidate.declarations.sdkRange); assertEquals(listOf(1), next.candidate.declarations.protocolRange)
            assertEquals("fixture", next.candidate.review.commands.single().path)
            owner.cancelVersionPreview(next.id); owner.installedVersions()
            val selected = owner.confirmInstalledVersion(preview(owner, root, b.publication.releaseID).id, true)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            owner.disableInstalledVersion(selected.pluginID, selected.observation.selection!!.revision, true)
            val rollback = preview(owner, root, a.publication.releaseID)
            assertEquals(false, rollback.previousEnabled); assertEquals(ExtensionController.PreviousVersionState.VERIFIED, rollback.previousState)
            assertEquals(listOf("ui.diff"), rollback.addedCapabilities); assertEquals(listOf("ui.table"), rollback.removedCapabilities)
            owner.confirmInstalledVersion(rollback.id, true)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun missingOrBadlySignedPreviousStaysUnknownWhileValidCandidateCanBeExplicitlySelected() = runBlocking<Unit> {
        for (missing in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val owner = f.controller(root, capabilities)
            try {
                val a = f.install(owner, root, "1.0.0", asset); val b = f.install(owner, root, "2.0.0", asset)
                val previous = if (missing) "b".repeat(64) else a.publication.releaseID
                if (!missing) File(root, "$previous/release.sig").let { Os.chmod(it.path, 384); it.writeBytes(ByteArray(64)); Os.chmod(it.path, 256) }
                checkNotNull(PluginReleaseCatalog.open(root)).use { it.change(a.snapshot.review.id, previous, false, it.selection().revision) }
                val value = preview(owner, root, b.publication.releaseID)
                assertEquals(ExtensionController.PreviousVersionState.UNKNOWN, value.previousState)
                assertNull(value.previous); assertNull(value.addedCapabilities); assertNull(value.removedCapabilities)
                assertNull(value.addedPermissions); assertNull(value.removedPermissions)
                assertEquals(listOf("session.rpc", "ui.table"), value.candidate.review.requiredCapabilities)
                owner.confirmInstalledVersion(value.id, true)
                assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
            } finally { owner.close(); PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun changedCandidateManifestCannotBorrowOldSignedCoreIdentityForDisplay() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root, capabilities)
        try {
            val b = f.install(owner, root, "2.0.0", asset)
            val file = File(root, "${b.publication.releaseID}/payload/manifest.json")
            Os.chmod(file.path, 384); file.appendText("\n"); Os.chmod(file.path, 256)
            assertTrue("unattested candidate declarations were displayed", runCatching { preview(owner, root, b.publication.releaseID) }.isFailure)
            assertFalse(File(root, PluginReleaseCatalog.SELECTION).exists()); assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
}

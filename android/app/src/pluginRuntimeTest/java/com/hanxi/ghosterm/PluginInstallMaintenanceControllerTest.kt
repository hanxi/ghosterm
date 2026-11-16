package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginInstallMaintenanceControllerTest {
    private val f = PluginJournalFixture
    @Test fun stagePreviewHasItsOwnAckAndNeitherInspectionNorCleanupLoadsAnySource() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = PluginVersionSelectionFixture.controller(root)
        try {
            val ref = f.create(root); assertEquals(PluginInstallJournal.State.ABANDONED, owner.installationMaintenance().entries.single().state)
            val preview = owner.previewStageRemoval(UUID.randomUUID().toString(), ref)
            assertEquals(ref, preview.stage); assertTrue(runCatching { owner.confirmVersionRemoval(preview.id, false) }.isFailure)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            val result = owner.confirmVersionRemoval(preview.id, true)
            assertTrue(result.toString(), result.complete); assertTrue(result.forgottenPackages.isEmpty()); assertEquals(ref, result.stage)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status); assertTrue(owner.installationMaintenance().entries.isEmpty())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun resolvedRecordRetirementHasIndependentAcknowledgementAndNeverDeletesTarget() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = PluginVersionSelectionFixture.controller(root)
        try {
            val ref = f.create(root); check(File(root, ref.name).renameTo(File(root, ref.target)))
            assertTrue(runCatching { owner.retireInstallationRecord(ref, false) }.isFailure); assertTrue(File(root, ref.recordName).exists())
            assertTrue(owner.retireInstallationRecord(ref, true)); assertEquals("unfinished public package bytes", File(root, "${ref.target}/partial").readText())
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancelledStagePreviewReleasesItsKnownTokenAndCannotBeCommitted() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = PluginVersionSelectionFixture.controller(root)
        try {
            val ref = f.create(root); val preview = owner.previewStageRemoval(UUID.randomUUID().toString(), ref)
            owner.cancelRemovalPreview(preview.id); owner.installationMaintenance()
            assertTrue(runCatching { owner.confirmVersionRemoval(preview.id, true) }.isFailure)
            val next = owner.previewStageRemoval(UUID.randomUUID().toString(), ref); owner.cancelRemovalPreview(next.id); owner.installationMaintenance()
            assertTrue(File(root, ref.name).isDirectory); assertTrue(File(root, ref.recordName).isFile)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
}

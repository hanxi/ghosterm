package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class PluginCatalogControllerTest {
    @Test fun realSignedInstallSurvivesControllerRestartButObservationNeverLoadsSelectionOrApprovesSource() = runBlocking {
        val root = PluginInstallTestFiles.root(); val first = PluginCatalogFixture.controller()
        try {
            val installed = PluginCatalogFixture.install(first, root)
            first.close()
            val second = PluginCatalogFixture.controller()
            try {
                val view = second.installedVersions(root); assertEquals(1, view.versions.size); assertTrue(view.issues.isEmpty())
                val declared = view.versions.single()
                assertEquals(installed.publication.releaseID, declared.releaseID); assertEquals(installed.snapshot.review.id, declared.pluginID)
                assertEquals(installed.snapshot.review.digest, declared.packageSHA256)
                checkNotNull(PluginReleaseCatalog.open(root)).use { it.change(declared.pluginID, declared.releaseID, true, null) }
                val reopened = second.installedVersions(root)
                assertTrue(reopened.selection!!.choices.getValue(declared.pluginID).enabled)
                assertEquals(ExtensionRegistry.Status.NOT_FOUND, second.resolve("fixture").status)
                // Even the explicit selection-storage call is no load/grant consumer.
                val review = second.review(File(installed.publication.directory, "payload"))
                assertEquals(ExtensionRegistry.Status.UNKNOWN, second.resolve("fixture", review.id, review.digest).status)
            } finally { second.close() }
        } finally { first.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun independentObserverKeepsParentSnapshotAndReportsBusyInsteadOfEmpty() = runBlocking {
        val root = PluginInstallTestFiles.root(); val parent = PluginCatalogFixture.controller()
        try {
            val installed = PluginCatalogFixture.install(parent, root)
            val reviewed = parent.review(File(installed.publication.directory, "payload"))
            val observer = parent.newReleaseInspector()
            try { assertEquals(1, observer.installedVersions(root).versions.size) } finally { observer.close() }
            val state = runCatching { parent.resolve("fixture", reviewed.id, reviewed.digest) }
            assertTrue("observation closed the active parent controller", state.isSuccess)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, state.getOrThrow().status)
            PluginReleaseStage.create(root, "f".repeat(64)).use {
                val error = runCatching { parent.installedVersions(root) }.exceptionOrNull()
                assertTrue("locked inventory was treated as empty", error is PluginReleaseCatalog.Rejected && error.reason == PluginReleaseCatalog.Reason.BUSY)
            }
        } finally { parent.close(); PluginInstallTestFiles.clean(root) }
    }
}

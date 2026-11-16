package com.hanxi.ghosterm

import android.system.Os
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.lifecycle.ViewModelProvider
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class PluginCatalogUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    @Test fun actualSettingsReadsSavedVersionsAndUnknownSelectionWithoutLoadingOrDeletingAnything() {
        val root = File(PluginCatalogFixture.context.filesDir, "plugin-releases")
        assertTrue("use a fresh owned test namespace", root.list()?.isEmpty() != false)
        val installer = PluginCatalogFixture.controller()
        try {
            val installed = runBlocking { PluginCatalogFixture.install(installer, root) }
            val unknown = File(root, ".prepare-unclassified").apply { check(mkdir()); File(this, "preserve").writeText("unknown public bytes") }
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").assertExists("saved versions entry missing").performScrollTo().performClick()
            fun ready() { ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-versions-count").fetchSemanticsNodes().isNotEmpty() } }
            ready()
            ui.onNodeWithTag("plugin-versions-count").assertTextContains("1 项", substring = true)
            ui.onNodeWithTag("plugin-versions-unverified").assertTextContains("尚未", substring = true)
            ui.onNodeWithTag("plugin-version-${installed.publication.releaseID}").assertExists()
            ui.onNodeWithTag("plugin-versions-no-selection").assertExists()
            checkNotNull(PluginReleaseCatalog.open(root)).use { it.change(installed.snapshot.review.id, installed.publication.releaseID, false, null) }
            ui.onNodeWithTag("plugin-versions-refresh").performScrollTo().performClick(); ready()
            ui.onNodeWithTag("plugin-versions-choice-${installed.snapshot.review.id}").assertTextContains("记录为禁用", substring = true)
            // Recreate the actual Activity, then explicitly reopen the list.
            ui.activityRule.scenario.recreate()
            ui.waitForIdle()
            if (ui.onAllNodesWithTag("plugin-versions-disclosure").fetchSemanticsNodes().isEmpty()) {
                ui.onNodeWithContentDescription("设置").performClick()
                ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            }
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); ready()
            ui.onNodeWithTag("plugin-versions-choice-${installed.snapshot.review.id}").assertTextContains("记录为禁用", substring = true)
            val record = File(root, PluginReleaseCatalog.SELECTION); Os.chmod(record.path, 384); record.writeText("{}"); Os.chmod(record.path, 256)
            ui.onNodeWithTag("plugin-versions-refresh").performScrollTo().performClick(); ready()
            ui.onNodeWithTag("plugin-versions-selection-unknown").assertExists()
            ui.onNodeWithTag("plugin-versions-no-selection").assertDoesNotExist()
            ui.onNodeWithTag("plugin-versions-maintenance").assertExists()
            assertEquals("unknown public bytes", File(unknown, "preserve").readText())
            assertEquals("{}", record.readText())
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, runBlocking { installer.resolve("fixture").status })
            val model = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
            ui.runOnIdle { assertTrue(model.entries.isEmpty()); assertNull(model.activeSession); assertNull(model.activeRuntime) }
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, runBlocking { model.controller!!.resolve("fixture").status })
        } finally {
            ui.activityRule.scenario.close(); runBlocking { installer.close() }
            // Wait for the independent observer to release its root, then clean
            // only this test's explicitly empty-at-start installation namespace.
            val end = android.os.SystemClock.elapsedRealtime() + 5000
            var closed = false
            while (!closed && android.os.SystemClock.elapsedRealtime() < end) {
                runCatching { PluginReleaseCatalog.open(root)?.close(); closed = true }
                if (!closed) android.os.SystemClock.sleep(10)
            }
            assertTrue("observer still owns fixture root", closed); PluginInstallTestFiles.clean(root)
        }
    }
}

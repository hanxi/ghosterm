package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class PluginInstallMaintenanceUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private fun await(tag: String) {
        try { ui.waitUntil(15000) { ui.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty() } }
        catch (_: ComposeTimeoutException) { ui.onNodeWithTag(tag).assertExists("expected maintenance result: $tag") }
    }
    @Test fun actualSettingsReviewsStageInventoryAndSeparatelyRetiresOnlyResolvedRecords() {
        val root = luaVersionDirectory(PluginVersionSelectionFixture.context)
        assertTrue("fresh owned namespace required", root.list()?.isEmpty() != false); if (!root.exists()) { check(root.mkdir()); android.system.Os.chmod(root.path, 448) }
        val abandoned = PluginJournalFixture.create(root); val absent = PluginJournalFixture.create(root, child = false)
        val model = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            ui.onNodeWithContentDescription("设置").performClick(); ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); await("plugin-install-maintenance-count")
            ui.onNodeWithTag("plugin-install-maintenance-count").assertTextContains("2 项", substring = true)
            ui.onNodeWithTag("plugin-install-maintenance-remove-${abandoned.id}").performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-removal-preview").assertTextContains("安装暂存", substring = true)
            ui.onNodeWithTag("plugin-removal-inventory").performScrollToNode(hasTestTag("plugin-removal-member-partial"))
            ui.onNodeWithTag("plugin-removal-confirm").assertIsNotEnabled(); ui.onNodeWithTag("plugin-removal-cancel").performScrollTo().performClick()
            assertTrue(File(root, abandoned.name).isDirectory)
            ui.onNodeWithTag("plugin-install-maintenance-remove-${abandoned.id}").performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-removal-acknowledge").assertIsOff().performScrollTo().performClick(); ui.onNodeWithTag("plugin-removal-confirm").performScrollTo().performClick(); await("plugin-removal-result")
            ui.onNodeWithTag("plugin-removal-result").assertTextContains("已处理完成", substring = true); assertFalse(File(root, abandoned.name).exists()); assertFalse(File(root, abandoned.recordName).exists())
            await("plugin-install-maintenance-count"); ui.onNodeWithTag("plugin-install-maintenance-count").assertTextContains("1 项", substring = true)
            ui.onNodeWithTag("plugin-install-maintenance-retire-${absent.id}").performScrollTo().performClick(); await("plugin-install-retire-preview")
            ui.onNodeWithTag("plugin-install-retire-confirm").assertIsNotEnabled(); ui.onNodeWithTag("plugin-install-retire-acknowledge").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-install-retire-confirm").performScrollTo().performClick(); await("plugin-install-retire-result")
            assertFalse(File(root, absent.recordName).exists()); assertTrue(root.list()!!.isEmpty())
            ui.runOnIdle { assertTrue(model.entries.isEmpty()); assertNull(model.activeSession); assertNull(model.activeRuntime) }
        } finally { ui.activityRule.scenario.close(); runBlocking { model.controller?.close() }; PluginInstallTestFiles.clean(root) }
    }
}

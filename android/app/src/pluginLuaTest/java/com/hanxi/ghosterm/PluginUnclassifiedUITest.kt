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
class PluginUnclassifiedUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private fun await(tag: String) {
        try { ui.waitUntil(15000) { ui.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty() } }
        catch (_: ComposeTimeoutException) { ui.onNodeWithTag(tag).assertExists("expected unknown-directory control: $tag") }
    }
    @Test fun actualSettingsRequiresFullInventoryAndSeparateUnknownAcknowledgementAfterEveryPreview() {
        val root = luaVersionDirectory(PluginVersionSelectionFixture.context)
        assertTrue("fresh owned namespace required", root.list()?.isEmpty() != false)
        if (!root.exists()) { check(root.mkdir()); android.system.Os.chmod(root.path, 448) }
        val folder = PluginUnknownFixture.directory(root); val model = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            ui.onNodeWithContentDescription("设置").performClick(); ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); await("plugin-unclassified-count")
            ui.onNodeWithTag("plugin-unclassified-count").assertTextContains("1 项", substring = true)
            ui.onNodeWithTag("plugin-unclassified-preview-0").performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-removal-preview").assertTextContains("归属未知", substring = true)
            ui.onNodeWithTag("plugin-removal-inventory").performScrollToNode(hasTestTag("plugin-removal-member-partial"))
            ui.onNodeWithTag("plugin-removal-acknowledge").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-removal-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-unclassified-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-removal-cancel").performScrollTo().performClick(); assertTrue(folder.exists())
            ui.onNodeWithTag("plugin-unclassified-preview-0").performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-unclassified-acknowledge").assertIsOff(); ui.onNodeWithTag("plugin-removal-acknowledge").assertIsOff()
            ui.onNodeWithTag("plugin-removal-acknowledge").performScrollTo().performClick(); ui.onNodeWithTag("plugin-unclassified-acknowledge").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-removal-confirm").performScrollTo().performClick(); await("plugin-removal-result")
            ui.onNodeWithTag("plugin-removal-result").assertTextContains("已处理完成", substring = true); assertFalse(folder.exists())
            await("plugin-unclassified-count"); ui.onNodeWithTag("plugin-unclassified-count").assertTextContains("0 项", substring = true)
            ui.runOnIdle { assertTrue(model.entries.isEmpty()); assertNull(model.activeSession); assertNull(model.activeRuntime) }
        } finally { ui.activityRule.scenario.close(); runBlocking { model.controller?.close() }; PluginInstallTestFiles.clean(root) }
    }
}

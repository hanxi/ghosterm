package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

/** Actual Settings page, actual signed fixture packages and real disk effects. */
@RunWith(AndroidJUnit4::class)
class PluginVersionRemovalUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun <T> main(body: () -> T): T {
        var value: T? = null; instrumentation.runOnMainSync { value = body() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(tag: String) { ui.waitUntil(15000) { ui.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty() } }
    @Test fun actualSettingsRequiresDisableInventoryAndSeparateDeleteConfirmationThenExplicitlyResumesSavedIntent() {
        val root = luaVersionDirectory(instrumentation.targetContext)
        assertTrue("fresh owned fixture namespace required", root.list()?.isEmpty() != false)
        val installer = PluginVersionSelectionFixture.controller(root)
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            val first = runBlocking { PluginVersionSelectionFixture.install(installer, root) }
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); await("plugin-versions-count")
            val owner = checkNotNull(main { extensions.controller })
            val selected = runBlocking {
                val view = owner.previewInstalledVersion(UUID.randomUUID().toString(), first.publication.releaseID, null, PluginVersionSelectionFixture.policy)
                owner.confirmInstalledVersion(view.id, true)
            }
            main { extensions.versionSelectionChanged(selected) }
            ui.onNodeWithTag("plugin-versions-refresh").performScrollTo().performClick(); await("plugin-version-disable-${selected.pluginID}")
            val remove = "plugin-version-remove-${first.publication.releaseID}"
            ui.onNodeWithTag(remove).assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-disable-${selected.pluginID}").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-disable-acknowledge").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-disable-confirm").performScrollTo().performClick(); await("plugin-version-selection-result")
            ui.waitUntil(15_000) { ui.onAllNodes(hasTestTag(remove) and isEnabled()).fetchSemanticsNodes().isNotEmpty() }
            ui.onNodeWithTag(remove).assertIsEnabled().performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-removal-summary").assertTextContains(first.publication.releaseID, substring = true)
            ui.onNodeWithTag("plugin-removal-inventory").performScrollToNode(hasTestTag("plugin-removal-member-release.sig"))
            ui.onNodeWithTag("plugin-removal-member-release.sig").assertTextContains("SHA-256", substring = true)
            ui.onNodeWithTag("plugin-removal-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-removal-cancel").performScrollTo().performClick()
            assertTrue(File(root, first.publication.releaseID).isDirectory)
            ui.onNodeWithTag(remove).performScrollTo().performClick(); await("plugin-removal-preview")
            ui.onNodeWithTag("plugin-removal-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-removal-confirm").performScrollTo().performClick(); await("plugin-removal-result")
            ui.onNodeWithTag("plugin-removal-result").assertTextContains("已处理完成", substring = true)
            assertFalse(File(root, first.publication.releaseID).exists()); assertTrue(main { extensions.entries.isEmpty() })
            val choice = checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().choices.getValue(selected.pluginID) }
            assertFalse(choice.enabled)
            val next = runBlocking { PluginVersionSelectionFixture.install(installer, root, "2.0.0") }
            val interrupted = runBlocking {
                val revision = checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().revision }
                val view = owner.previewVersionRemoval(UUID.randomUUID().toString(), next.publication.releaseID, revision)
                owner.confirmVersionRemoval(view.id, true, checkpoint = { if (it == "after-detach") throw IOException("controlled UI fixture interruption") })
            }
            assertFalse(interrupted.complete); val raw = File(root, PluginReleaseCatalog.REMOVAL).readBytes()
            ui.onNodeWithTag("plugin-versions-refresh").performScrollTo().performClick(); await("plugin-removal-resume")
            assertArrayEquals(raw, File(root, PluginReleaseCatalog.REMOVAL).readBytes())
            ui.onNodeWithTag("plugin-removal-resume").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-removal-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-removal-resume").performScrollTo().performClick(); await("plugin-removal-result")
            ui.onNodeWithTag("plugin-removal-result").assertTextContains("已处理完成", substring = true)
            assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists()); assertFalse(File(root, next.publication.releaseID).exists())
            assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
        } finally {
            ui.activityRule.scenario.close(); runBlocking { installer.close(); extensions.controller?.close() }
            val deadline = SystemClock.elapsedRealtime() + 5000; var released = false
            while (!released && SystemClock.elapsedRealtime() < deadline) {
                runCatching { PluginReleaseCatalog.open(root)?.close(); released = true }; if (!released) SystemClock.sleep(10)
            }
            assertTrue("observer retained install root", released); PluginInstallTestFiles.clean(root)
        }
    }
}

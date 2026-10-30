package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real Activity/ViewModelStore and navigation; no test-owned retained clipboard. */
@RunWith(AndroidJUnit4::class)
class FileClipboardLifecycleTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private lateinit var file:File
    private fun files()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    @Before fun setup() {
        file=File(ui.activity.filesDir,"home/clipboard-${UUID.randomUUID()}.txt")
        file.parentFile!!.mkdirs();file.writeText("clipboard lifecycle")
    }
    @After fun cleanup() {
        ui.runOnIdle { files().leave();sessions().sessions.map { it.id }.forEach(sessions()::close) };file.delete()
    }
    @Test fun clipboardSurvivesSettingsTerminalAndActivityRecreationWithoutOwningShell() {
        ui.onNodeWithText("本机文件").performClick();ui.waitUntil { !files().loading }
        ui.onNodeWithTag("file-entry:${file.name}").performScrollTo().performTouchInput { longClick() }
        ui.onNodeWithTag("file-copy").performClick();val snapshot=files().clipboard!!
        ui.activityRule.scenario.recreate();assertSame(snapshot,files().clipboard)
        ui.onNodeWithText("查看清单").performClick();ui.activityRule.scenario.recreate()
        ui.onNodeWithTag("file-clipboard-item:${snapshot.items.single().id}").assertIsDisplayed()
        ui.onNodeWithText("返回文件").performClick();ui.onNodeWithText("返回").performClick()
        ui.onNodeWithText("设置").performClick();assertSame(snapshot,files().clipboard)
        ui.onNodeWithText("返回").performClick();ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10000) { sessions().active?.state?.connected==true }
        val terminal=sessions().active!!
        ui.onNodeWithTag("terminal-files").performClick();ui.waitUntil { !files().loading }
        assertSame(snapshot,files().clipboard);assertSame(terminal,sessions().active)
        ui.onNodeWithTag("file-clipboard").assertIsDisplayed()
        ui.onNodeWithText("返回").performClick();assertSame(terminal,sessions().active)
        assertTrue(terminal.state.connected);assertEquals("clipboard lifecycle",file.readText())
    }
}

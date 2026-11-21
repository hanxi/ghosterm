package com.hanxi.ghosterm

import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginViewsUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun await(check: () -> Boolean) {
        var observed = false
        try { ui.waitUntil(5000, check); observed = true } catch (_: ComposeTimeoutException) { }
        assertTrue("Expected registered Native view state", observed)
    }
    private fun fixture(dark: Boolean = false, scale: Float = 1f, block: (PluginViewsModel, PluginViewsFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val f = PluginViewsFixture()
        lateinit var m: PluginViewsModel; ui.runOnIdle { m = f.populated(scope) }
        ui.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, scale)) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    Surface { Column(Modifier.verticalScroll(rememberScrollState())) { PluginViewsContent(m) } }
                }
            }
        }
        try { await { (m.state.value.items.last().content as PluginViewItem.Content.Task).model.state.value.snapshot != null }; block(m, f) }
        finally { ui.runOnIdle { m.invalidate(); f.closeRelease.complete(Unit); scope.cancel() } }
    }
    @Test fun actualCloseKeepsOtherTableAndReopenGetsFreshHandle() = fixture { m, f ->
        val alpha = (m.state.value.items[0].content as PluginViewItem.Content.Table).model
        val beta = (m.state.value.items[1].content as PluginViewItem.Content.Table).model
        ui.onAllNodesWithTag("plugin-view-close").assertCountEquals(4)
        ui.onAllNodesWithTag("plugin-table-next")[0].performScrollTo().performClick()
        await { alpha.state.value.page?.offset == 16 }; assertEquals(0, beta.state.value.page?.offset)
        val close = ui.onNodeWithContentDescription("关闭视图 table-a，不取消任务").performScrollTo()
        ui.runOnIdle { f.pauseClose = true }
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        instrumentation.sendKeyDownUpSync(KeyEvent.KEYCODE_TAB)
        close.performSemanticsAction(SemanticsActions.RequestFocus).assertIsFocused()
        instrumentation.sendKeyDownUpSync(KeyEvent.KEYCODE_ENTER)
        await { f.closeEntered.isCompleted && m.state.value.items.first().closing }
        ui.onNodeWithContentDescription("关闭视图 table-b，不取消任务").assert(
            SemanticsMatcher.expectValue(SemanticsProperties.Focused, true)
        ) { "Adjacent close button must gain keyboard focus before the close reply" }
        ui.runOnIdle { f.closeRelease.complete(Unit) }
        await { m.state.value.items.size == 3 }
        ui.onNodeWithContentDescription("关闭视图 table-b，不取消任务").assertIsFocused()
        assertTrue(!alpha.state.value.valid && m.state.value.items.any { (it.content as? PluginViewItem.Content.Table)?.model === beta })
        ui.runOnIdle { f.entries += f.entry(5, "table-a", "table", "alpha"); f.revision++; m.apply(f.data()) }
        ui.onAllNodesWithTag("plugin-view-close").assertCountEquals(4)
        val fresh = (m.state.value.items.last().content as PluginViewItem.Content.Table).model
        await { fresh.state.value.page != null }; assertEquals(0, fresh.state.value.page?.offset)
    }
    @Test fun closeTaskUnmountsPollingWithoutSendingCancel() = fixture { m, f ->
        ui.onNodeWithContentDescription("关闭视图 task-a，不取消任务").performScrollTo().performClick()
        await { m.state.value.items.size == 3 }; val calls = f.task.calls.size
        ui.mainClock.advanceTimeBy(2000); ui.waitForIdle()
        assertTrue(f.task.calls.size == calls && f.task.calls.none { it.first == "view.task.cancel" })
    }
    @Test fun unconfirmedCloseRetainsWidgetAndRestartsPolling() = fixture { m, f ->
        val item = m.state.value.items.last(); val task = (item.content as PluginViewItem.Content.Task).model
        ui.onNodeWithText("task-a").assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.Heading))
        ui.runOnIdle { f.pauseClose = true; f.closeFails = true }
        ui.onNodeWithContentDescription("关闭视图 task-a，不取消任务").performScrollTo().performClick()
        await { item.closing && f.closeEntered.isCompleted }
        ui.onNodeWithContentDescription("正在关闭视图 task-a").assertIsNotEnabled()
        ui.mainClock.advanceTimeBy(600); ui.waitForIdle()
        ui.runOnIdle { f.task.state.put("completed", "5").put("sequence", "2"); f.closeRelease.complete(Unit) }
        await { item.closeIssue != null && !item.closing }
        ui.onNodeWithTag("plugin-view-close-issue")
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.LiveRegion, LiveRegionMode.Polite))
        ui.mainClock.advanceTimeBy(600); await { task.state.value.snapshot?.completed == "5" }
        assertTrue(m.state.value.items.size == 4 && f.task.calls.none { it.first == "view.task.cancel" })
    }
    private fun painted(dark: Boolean, scale: Float) = fixture(dark, scale) { m, _ ->
        ui.onNodeWithText("table-a").performScrollTo().assertIsDisplayed()
            .assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.Heading))
        val button = ui.onNodeWithContentDescription("关闭视图 table-a，不取消任务")
            .performScrollTo().assertIsDisplayed().assertIsEnabled().assertHasClickAction()
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.Role, Role.Button))
        val bitmap = button.captureToImage().asAndroidBitmap(); val colors = mutableSetOf<Int>()
        for (y in 0 until bitmap.height) for (x in 0 until bitmap.width) colors += bitmap.getPixel(x, y)
        assertTrue("Native close control must paint readable glyph pixels", colors.size > 8)
        button.performClick(); await { m.state.value.items.size == 3 }
    }
    @Test fun lightNormalTextPaints() = painted(false, 1f)
    @Test fun lightLargeTextPaints() = painted(false, 2f)
    @Test fun darkNormalTextPaints() = painted(true, 1f)
    @Test fun darkLargeTextPaints() = painted(true, 2f)
}

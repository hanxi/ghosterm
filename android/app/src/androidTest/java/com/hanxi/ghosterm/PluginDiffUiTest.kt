package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginDiffUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun fixture(empty: Boolean = false, dark: Boolean = false, scale: Float = 1f, block: (PluginDiffModel, PluginDiffFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val f = PluginDiffFixture(); if (empty) f.empty()
        lateinit var model: PluginDiffModel
        ui.runOnIdle { model = PluginDiffModel(scope, f::request) }
        ui.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, scale)) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    Surface { Column(Modifier.verticalScroll(rememberScrollState())) { PluginDiffContent(model) } }
                }
            }
        }
        try { ui.waitUntil(5000) { model.state.value.page != null || model.state.value.issue != null }; block(model, f) }
        finally { ui.runOnIdle { model.invalidate(); f.release.complete(Unit); scope.cancel() } }
    }
    @Test fun realPagingButtonsShowNextAndPreviousExactTextAndEndings() = fixture { model, _ ->
        ui.onNodeWithTag("plugin-diff-next").performScrollTo().performClick()
        ui.runOnIdle { assertEquals("Next button must select the next verified page", 3, model.state.value.selectedOffset) }
        ui.waitUntil(5000) { model.state.value.page?.offset == 3 }
        ui.onNodeWithTag("plugin-diff-text-3").performScrollTo().assertTextEquals("gamma").assertContentDescriptionEquals("原文无此行，新第 2 行")
        ui.onNodeWithTag("plugin-diff-next").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithTag("plugin-diff-previous").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.page?.offset == 0 }
        ui.onNodeWithTag("plugin-diff-text-0").performScrollTo().assertTextEquals("alpha")
        ui.onNodeWithTag("plugin-diff-ending-0").performScrollTo().assertTextEquals("CRLF").assertContentDescriptionEquals("换行：CRLF")
    }
    @Test fun emptyDiffAndDisabledPagingAreNativeSemantics() = fixture(empty = true) { _, f ->
        ui.onNodeWithTag("plugin-diff-empty").assertTextEquals("两份文本均为空")
        ui.onNodeWithTag("plugin-diff-next").assertIsNotEnabled(); ui.onNodeWithTag("plugin-diff-previous").assertIsNotEnabled()
        assertEquals(2, f.calls.size)
    }
    @Test fun scrollActionsKeepBothAxesIndependentlyAddressable() = fixture { _, _ ->
        val scrolls = ui.onAllNodes(hasScrollAction(), useUnmergedTree = true).fetchSemanticsNodes()
        val horizontal = SemanticsProperties.HorizontalScrollAxisRange; val vertical = SemanticsProperties.VerticalScrollAxisRange
        assertTrue("Each axis needs its own addressable scroll action", scrolls.any { it.config.contains(horizontal) } &&
            scrolls.any { it.config.contains(vertical) } && scrolls.none { it.config.contains(horizontal) && it.config.contains(vertical) })
    }
    @Test fun loadingPageDisablesControlsAndRemovesPriorText() = fixture { model, f ->
        ui.runOnIdle { f.pause = "view.diff.page" }
        ui.onNodeWithTag("plugin-diff-next").performScrollTo().performClick()
        ui.waitUntil(5000) { f.entered.isCompleted }
        ui.onNodeWithTag("plugin-diff-text-0").assertDoesNotExist()
        ui.onNodeWithTag("plugin-diff-next").assertIsNotEnabled(); ui.onNodeWithTag("plugin-diff-reload").assertIsNotEnabled()
        ui.runOnIdle { f.release.complete(Unit) }; ui.waitUntil(5000) { model.state.value.page?.offset == 3 }
    }
    private fun painted(dark: Boolean, scale: Float) = fixture(dark = dark, scale = scale) { model, _ ->
        val text = ui.onNodeWithTag("plugin-diff-text-0").performScrollTo().assertTextEquals("alpha").assertIsDisplayed()
        val bitmap = text.captureToImage().asAndroidBitmap(); val colors = mutableSetOf<Int>()
        for (y in 0 until bitmap.height) for (x in 0 until bitmap.width) colors += bitmap.getPixel(x, y)
        assertTrue("Native exact text must draw visible glyph pixels", colors.size > 8)
        ui.onNodeWithTag("plugin-diff-ending-0").performScrollTo().assertTextEquals("CRLF").assertIsDisplayed().assertContentDescriptionEquals("换行：CRLF")
        ui.onNodeWithTag("plugin-diff-next").performScrollTo().assertIsDisplayed().performClick()
        ui.waitUntil(5000) { model.state.value.page?.offset == 3 }
        ui.onNodeWithTag("plugin-diff-text-3").performScrollTo().assertTextEquals("gamma")
    }
    @Test fun lightNormalTextPaintsAndRemainsAccessible() = painted(false, 1f)
    @Test fun lightLargeTextPaintsAndRemainsAccessible() = painted(false, 2f)
    @Test fun darkNormalTextPaintsAndRemainsAccessible() = painted(true, 1f)
    @Test fun darkLargeTextPaintsAndRemainsAccessible() = painted(true, 2f)
}

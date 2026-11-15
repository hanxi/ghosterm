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
class PluginTableUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun fixture(total: Int = 1000000, dark: Boolean = false, scale: Float = 1f, block: (PluginTableModel, PluginTableFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val f = PluginTableFixture(); f.description.put("total", total)
        lateinit var model: PluginTableModel
        ui.runOnIdle { model = PluginTableModel(scope, f::request) }
        ui.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, scale)) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    Surface { Column(Modifier.verticalScroll(rememberScrollState())) { PluginTableContent(model) } }
                }
            }
        }
        try { ui.waitUntil(5000) { model.state.value.page != null || model.state.value.issue != null }; block(model, f) }
        finally { ui.runOnIdle { model.invalidate(); f.release.complete(Unit); scope.cancel() } }
    }
    @Test fun realPagingButtonsShowNextLastAndPreviousExactValues() = fixture { model, _ ->
        ui.onNodeWithTag("plugin-table-next").performScrollTo().performClick()
        ui.runOnIdle { assertEquals("Next button must select the next bounded page", 16, model.state.value.selectedOffset) }
        ui.waitUntil(5000) { model.state.value.page?.offset == 16 }
        ui.onNodeWithTag("plugin-table-cell-17-n").performScrollTo().assertTextEquals("17").assertContentDescriptionEquals("Number，第 17 行")
        ui.onNodeWithTag("plugin-table-last").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.page?.offset == 999984 }
        ui.onNodeWithTag("plugin-table-cell-1000000-cube").performScrollTo().assertTextEquals("1000000000000000000")
        ui.onNodeWithTag("plugin-table-next").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithTag("plugin-table-previous").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.page?.offset == 999968 }
        assertTrue(model.state.value.page?.rows?.size == 16)
    }
    @Test fun emptyTableAndDisabledPagingAreNativeSemantics() = fixture(total = 0) { _, f ->
        ui.onNodeWithTag("plugin-table-empty").assertTextEquals("空表")
        ui.onNodeWithTag("plugin-table-next").assertIsNotEnabled(); ui.onNodeWithTag("plugin-table-previous").assertIsNotEnabled()
        assertEquals(2, f.calls.size)
    }
    @Test fun scrollActionsKeepBothAxesIndependentlyAddressable() = fixture(total = 17) { _, _ ->
        val scrolls = ui.onAllNodes(hasScrollAction(), useUnmergedTree = true).fetchSemanticsNodes()
        val horizontal = SemanticsProperties.HorizontalScrollAxisRange
        val vertical = SemanticsProperties.VerticalScrollAxisRange
        assertTrue("Each axis needs its own addressable scroll action", scrolls.any { it.config.contains(horizontal) } &&
            scrolls.any { it.config.contains(vertical) } && scrolls.none { it.config.contains(horizontal) && it.config.contains(vertical) })
    }
    @Test fun loadingPageDisablesControlsAndRemovesPriorCells() = fixture { model, f ->
        ui.runOnIdle { f.pause = "view.table.page" }
        ui.onNodeWithTag("plugin-table-next").performScrollTo().performClick()
        ui.waitUntil(5000) { f.entered.isCompleted }
        ui.onNodeWithTag("plugin-table-cell-1-n").assertDoesNotExist()
        ui.onNodeWithTag("plugin-table-next").assertIsNotEnabled()
        ui.onNodeWithTag("plugin-table-reload").assertIsNotEnabled()
        ui.runOnIdle { f.release.complete(Unit) }
        ui.waitUntil(5000) { model.state.value.page?.offset == 16 }
    }
    @Test fun darkLargeTextKeepsHeadersCellsCollectionAndPagingAccessible() = fixture(dark = true, scale = 2f, total = 17) { model, _ ->
        ui.onNodeWithTag("plugin-table-column-n").performScrollTo().assertTextEquals("Number")
            .assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.Heading))
        ui.onNodeWithTag("plugin-table-cell-1-n").performScrollTo().assertTextEquals("1").assertIsDisplayed()
        ui.onNodeWithTag("plugin-table-grid").assert(SemanticsMatcher.keyIsDefined(SemanticsProperties.CollectionInfo))
        val bitmap = ui.onNodeWithTag("plugin-table-grid").captureToImage().asAndroidBitmap()
        val colors = mutableSetOf<Int>()
        for (y in 0 until bitmap.height step 2) for (x in 0 until bitmap.width step 2) colors += bitmap.getPixel(x, y)
        assertTrue("Native grid must draw text pixels", colors.size > 8)
        ui.onNodeWithTag("plugin-table-next").performScrollTo().assertIsDisplayed().performClick()
        ui.waitUntil(5000) { model.state.value.page?.offset == 16 }
        ui.onNodeWithTag("plugin-table-cell-17-n").performScrollTo().assertTextEquals("17")
    }
}

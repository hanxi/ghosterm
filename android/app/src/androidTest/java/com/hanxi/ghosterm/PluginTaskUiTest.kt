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
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginTaskUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun awaitState(check: () -> Boolean) {
        var reached = false
        try { ui.waitUntil(5000, check); reached = true } catch (_: ComposeTimeoutException) { }
        assertTrue("Expected observed Native task state", reached)
    }
    private fun fixture(dark: Boolean = false, scale: Float = 1f, prepare: (PluginTaskFixture) -> Unit = {}, block: (PluginTaskModel, PluginTaskFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val f = PluginTaskFixture(); prepare(f)
        lateinit var model: PluginTaskModel; ui.runOnIdle { model = PluginTaskModel(scope, f::request) }
        ui.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, scale)) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    Surface { Column(Modifier.verticalScroll(rememberScrollState())) { PluginTaskContent(model) } }
                }
            }
        }
        try { awaitState { model.state.value.snapshot != null || model.state.value.issue != null }; block(model, f) }
        finally { ui.runOnIdle { model.invalidate(); f.release.complete(Unit); scope.cancel() } }
    }
    @Test fun actualCancelButtonAcknowledgesWithoutInventingStop() = fixture { model, f ->
        ui.onNodeWithTag("plugin-task-cancel").performScrollTo().performClick()
        awaitState { model.state.value.cancellation == PluginTaskModel.Cancellation.REQUESTED }
        ui.onNodeWithTag("plugin-task-cancel").assertIsNotEnabled()
        ui.onNodeWithTag("plugin-task-phase").assertTextEquals("正在运行")
        ui.onNodeWithTag("plugin-task-cancel-status")
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.LiveRegion, LiveRegionMode.Polite))
        assertEquals(1, f.calls.count { it.first == "view.task.cancel" })
    }
    @Test fun unknownCancelOnlyAllowsExplicitRead() = fixture { model, f ->
        ui.runOnIdle { f.unknownMethod = "view.task.cancel" }
        ui.onNodeWithTag("plugin-task-cancel").performScrollTo().performClick()
        awaitState { model.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED }
        ui.onNodeWithTag("plugin-task-cancel").assertIsNotEnabled()
        ui.runOnIdle { f.finish() }
        ui.onNodeWithTag("plugin-task-read").performScrollTo().performClick()
        awaitState { model.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.CANCELLED }
        assertTrue(model.state.value.cancellation == PluginTaskModel.Cancellation.UNCONFIRMED && f.calls.count { it.first == "view.task.cancel" } == 1)
    }
    @Test fun knownZeroIsNotUnknown() = fixture(prepare = { f ->
        f.state.put("completed", "0").put("total", JSONObject().put("kind", "known").put("units", "0")).put("phase", "succeeded")
            .put("result", JSONObject().put("kind", "success").put("summary", "0 primes"))
    }) { _, _ -> ui.onNodeWithTag("plugin-task-count").assertTextEquals("0 / 0 · candidates"); ui.onNodeWithTag("plugin-task-cancel").assertIsNotEnabled() }
    @Test fun unknownTotalIsNotZero() = fixture(prepare = { it.state.put("total", JSONObject().put("kind", "unknown")) }) { _, _ ->
        ui.onNodeWithTag("plugin-task-count").assertTextEquals("4 · 总量未知 · candidates")
    }
    @Test fun mountedViewRefreshesPausesAndResumesAfterExplicitRead() = fixture { model, f ->
        ui.runOnIdle { f.state.put("completed", "5").put("sequence", "2") }
        ui.mainClock.advanceTimeBy(600); awaitState { model.state.value.snapshot?.completed == "5" }
        ui.runOnIdle { f.unknownMethod = "view.task.read" }
        ui.mainClock.advanceTimeBy(600); awaitState { model.state.value.issue != null }
        ui.onNodeWithTag("plugin-task-issue")
            .assert(SemanticsMatcher.expectValue(SemanticsProperties.LiveRegion, LiveRegionMode.Polite))
        val count = f.calls.size; ui.mainClock.advanceTimeBy(1200); ui.waitForIdle()
        assertTrue(f.calls.size == count && !model.canAutoRefresh)
        ui.runOnIdle { f.unknownMethod = null; f.state.put("completed", "6").put("sequence", "3") }
        ui.onNodeWithTag("plugin-task-read").performScrollTo().performClick(); awaitState { model.state.value.snapshot?.completed == "6" }
        ui.runOnIdle { f.state.put("completed", "7").put("sequence", "4") }
        ui.mainClock.advanceTimeBy(600); awaitState { model.state.value.snapshot?.completed == "7" }
    }
    private fun painted(dark: Boolean, scale: Float) = fixture(dark, scale) { model, _ ->
        val text = ui.onNodeWithTag("plugin-task-message").performScrollTo().assertTextEquals("Counting primes").assertIsDisplayed()
        val bitmap = text.captureToImage().asAndroidBitmap(); val colors = mutableSetOf<Int>()
        for (y in 0 until bitmap.height) for (x in 0 until bitmap.width) colors += bitmap.getPixel(x, y)
        assertTrue("Native task must paint readable glyph pixels", colors.size > 8)
        ui.onNodeWithTag("plugin-task-count").performScrollTo().assertTextEquals("4 / 100 · candidates").assertIsDisplayed()
        ui.onNodeWithTag("plugin-task-cancel").performScrollTo().assertIsDisplayed().performClick()
        awaitState { model.state.value.cancellation == PluginTaskModel.Cancellation.REQUESTED }
    }
    @Test fun lightNormalTextPaints() = painted(false, 1f)
    @Test fun lightLargeTextPaints() = painted(false, 2f)
    @Test fun darkNormalTextPaints() = painted(true, 1f)
    @Test fun darkLargeTextPaints() = painted(true, 2f)
}

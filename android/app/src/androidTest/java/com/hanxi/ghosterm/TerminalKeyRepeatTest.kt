package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import java.util.concurrent.atomic.AtomicInteger

class TerminalKeyRepeatTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val enabled = mutableStateOf(true)
    private val visible = mutableStateOf(true)
    private val count = AtomicInteger()
    private fun awaitRepeat(condition: () -> Boolean) {
        try { ui.waitUntil(3000,condition) }
        catch (failure: ComposeTimeoutException) { fail("A held terminal key must produce repeated input") }
    }
    private fun setup() {
        ui.setContent { MaterialTheme {
            if (visible.value) RepeatingTerminalButton(enabled.value, { count.incrementAndGet() }, Modifier.testTag("repeat")) { Text("←") }
        } }
    }
    @Test fun tapAndAccessibilityClickSendExactlyOneKeyEach() {
        setup()
        ui.onNodeWithTag("repeat").performTouchInput { click() }
        ui.waitForIdle(); assertEquals(1,count.get())
        ui.onNodeWithTag("repeat").performClick()
        ui.waitForIdle(); assertEquals(2,count.get())
    }
    @Test fun holdingRepeatsAndReleaseAddsNoKeyAndStops() {
        setup()
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() >= 4 }
        ui.waitForIdle()
        ui.mainClock.autoAdvance=false
        val released=count.get()
        try {
            ui.onNodeWithTag("repeat").performTouchInput { up() }
            ui.mainClock.advanceTimeBy(250)
            assertEquals("Release sends no extra key and cancels future repeats",released,count.get())
        } finally { ui.mainClock.autoAdvance=true }
        ui.onNodeWithTag("repeat").performTouchInput { click() }
        ui.waitForIdle(); assertEquals(released+1,count.get())
    }
    @Test fun draggingOutsideCancelsTheHeldButton() {
        setup()
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() >= 3 }
        ui.onNodeWithTag("repeat").performTouchInput { moveTo(androidx.compose.ui.geometry.Offset(-500f,-500f)) }
        ui.waitForIdle(); val moved=count.get()
        ui.mainClock.advanceTimeBy(250);ui.waitForIdle();assertEquals(moved,count.get())
        ui.onNodeWithTag("repeat").performTouchInput { up() }
        ui.waitForIdle();assertEquals(moved,count.get())
    }
    @Test fun losingWindowFocusStopsRepeating() {
        setup()
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() >= 3 }
        lateinit var dialog: android.app.AlertDialog
        ui.runOnIdle { dialog=android.app.AlertDialog.Builder(ui.activity).setMessage("焦点对照").show() }
        try {
            ui.waitUntil(3000) { !ui.activity.hasWindowFocus() }
            ui.waitForIdle();val unfocused=count.get()
            ui.mainClock.advanceTimeBy(250);ui.waitForIdle();assertEquals(unfocused,count.get())
        } finally { ui.runOnIdle { dialog.dismiss() } }
        ui.onNodeWithTag("repeat").performTouchInput { cancel() }
    }
    @Test fun cancellationAndDisablingAndRemovalStopRepeating() {
        setup()
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() >= 3 }
        ui.onNodeWithTag("repeat").performTouchInput { cancel() }
        ui.waitForIdle(); val cancelled = count.get()
        ui.mainClock.advanceTimeBy(250);ui.waitForIdle(); assertEquals(cancelled,count.get())
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() > cancelled+2 }
        ui.runOnIdle { enabled.value=false }; ui.waitForIdle(); val disabled=count.get()
        ui.mainClock.advanceTimeBy(250);ui.waitForIdle(); assertEquals(disabled,count.get())
        ui.onNodeWithTag("repeat").performTouchInput { up() }
        ui.runOnIdle { enabled.value=true }
        ui.onNodeWithTag("repeat").performTouchInput { down(center) }
        awaitRepeat { count.get() > disabled+2 }
        ui.runOnIdle { visible.value=false }; ui.waitForIdle(); val removed=count.get()
        ui.mainClock.advanceTimeBy(250);ui.waitForIdle(); assertEquals(removed,count.get())
    }
}

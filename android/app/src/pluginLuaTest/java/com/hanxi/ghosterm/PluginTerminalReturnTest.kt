package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginTerminalReturnTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val probe by lazy { PluginTerminalReturnProbe(ui) }
    @After fun clean() = probe.close()
    @Test fun shownKeyboardAndSamePtyReturnAfterToolbarAndSystemBackFromPlugins() = exercise(true)
    @Test fun hiddenKeyboardAndSamePtyReturnAfterToolbarAndSystemBackFromPlugins() = exercise(false)
    private fun exercise(shown: Boolean) {
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        val original = probe.begin()
        for (systemBack in listOf(false, true)) {
            probe.leaveToPlugins(shown)
            assertSame(original, ui.runOnIdle { probe.sessions.active }); assertTrue(original.connection.state().connected)
            probe.returnFromPlugins(original, shown, systemBack)
            probe.inputProof(original)
        }
    }
    @Test fun returnVisibilityIsConsumedBeforeActivityRecreation() {
        val original = probe.begin()
        probe.leaveToPlugins(true); probe.returnFromPlugins(original, true)
        probe.keyboard(false)
        ui.activityRule.scenario.recreate()
        probe.assertReturned(original, false); probe.inputProof(original)
    }
    @Test fun filePageReturnKeepsSameSessionAndBothKeyboardStates() {
        val original = probe.begin()
        for (shown in listOf(true, false)) {
            probe.keyboard(shown); ui.openTerminalFiles()
            ui.onNodeWithTag("file-location").assertIsDisplayed()
            ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
            probe.assertReturned(original, shown); probe.inputProof(original)
        }
    }
    @Test fun pendingReturnBelongsToOriginalUuidAcrossAnotherLiveTerminal() {
        val original = probe.begin(); probe.leaveToPlugins(true)
        repeat(2) { ui.onNodeWithContentDescription("返回").performClick() }
        ui.runOnIdle { probe.sessions.newLocal() }
        probe.await("another actual PTY did not attach") { ui.runOnIdle {
            probe.sessions.active?.let { it !== original && it.state.connected && probe.terminal().surface === it.surface } == true
        } }
        val other = ui.runOnIdle { probe.sessions.active!! }; probe.own(other)
        probe.assertReturned(other, false); probe.inputProof(other)
        probe.leaveToPlugins(false); probe.returnFromPlugins(other, false)
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-session-select-${original.id}").performClick()
        probe.assertReturned(original, true); probe.inputProof(original)
        assertTrue(other.connection.state().connected)
    }
    @Test fun ptyStoppedWhilePluginPageOpenReturnsWithoutInputOrIme() {
        val original = probe.begin(); probe.leaveToPlugins(true)
        ui.runOnIdle { original.connection.commit("exit 7\r") }
        probe.await("the actual original shell did not exit") { ui.runOnIdle { original.state.terminal } }
        probe.returnFromPlugins(original, false)
        ui.runOnIdle {
            assertFalse("stopped return must keep the native input view disabled", probe.terminal().inputEnabled)
            assertFalse(probe.terminal().commitText("must-not-send"))
        }
    }
}

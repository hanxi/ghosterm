package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.ComposeContentTestRule
import androidx.test.platform.app.InstrumentationRegistry
import java.io.FileInputStream

/** Exercise the visible fallback, including popup dismissal and window focus. */
internal fun ComposeContentTestRule.showTerminalKeyboard() {
    onNodeWithTag("terminal-keyboard").assertDoesNotExist()
    onNodeWithTag("terminal-menu").performClick()
    onNodeWithTag("terminal-menu-keyboard").performScrollTo().assertIsEnabled().performClick()
    try { waitUntil(10_000) {
        var inset = false
        InstrumentationRegistry.getInstrumentation().runOnMainSync {
            inset = androidx.test.runner.lifecycle.ActivityLifecycleMonitorRegistry.getInstance()
                .getActivitiesInStage(androidx.test.runner.lifecycle.Stage.RESUMED).any {
                    androidx.core.view.ViewCompat.getRootWindowInsets(it.window.decorView)
                        ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()) == true
                }
        }
        inset && terminalImeShown()
    } } catch (failure: ComposeTimeoutException) {
        throw AssertionError("Keyboard menu did not display the actual system IME", failure)
    }
    waitForIdle()
}

internal fun terminalImeShown(): Boolean = InstrumentationRegistry.getInstrumentation()
    .uiAutomation.executeShellCommand("dumpsys input_method").use {
        FileInputStream(it.fileDescriptor).bufferedReader().use { reader ->
            "mInputShown=true" in reader.readText()
        }
    }

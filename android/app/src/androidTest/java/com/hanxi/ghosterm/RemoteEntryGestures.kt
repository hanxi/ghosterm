package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.AndroidComposeTestRule
import androidx.test.ext.junit.rules.ActivityScenarioRule

/** Ordinary-authentication tests explicitly choose the no-probe shortcut.
 * Default host discovery is exercised by TmuxProductTest. */
fun AndroidComposeTestRule<ActivityScenarioRule<MainActivity>, MainActivity>.openOrdinaryHost(alias: String) {
    onNodeWithTag("host-menu:"+alias).performScrollTo().performClick()
    onNodeWithTag("host-ordinary-"+alias).performScrollTo().performClick()
}

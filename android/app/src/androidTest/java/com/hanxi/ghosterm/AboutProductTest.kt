package com.hanxi.ghosterm

import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AboutProductTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private fun model() = ViewModelProvider(ui.activity)[AboutViewModel::class.java]
    @Test fun actualBuildAndPackagedLicensesHaveOneBackPathAndSurviveRecreation() {
        val raw = ui.activity.assets.open("ghosterm-build.json").bufferedReader().use { it.readText() }
        val expected = AboutBuild.decode(raw)
        InstrumentationRegistry.getArguments().getString("aboutExpectedCommit")?.let { assertEquals(it, expected.commit) }
        InstrumentationRegistry.getArguments().getString("aboutExpectedState")?.let { assertEquals(it, expected.sourceState) }
        assertNotNull("Fixture: this repository build must have a commit", expected.commit)
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("关于 Ghosterm").performScrollTo().performClick()
        ui.waitUntil(10_000) { ui.runOnIdle { model().build != null } }
        ui.onNodeWithTag("about-version").assertTextEquals("版本 ${BuildConfig.VERSION_NAME}")
        ui.onNodeWithTag("about-commit").assertTextEquals(expected.commit!!)
        ui.onNodeWithTag("about-source-state").assertTextEquals(expected.statusText)
        ui.waitUntil(10_000) { ViewModelProvider(ui.activity)[RuntimeInfoViewModel::class.java].state.value is RuntimeInfo.Ready }
        val runtime = ViewModelProvider(ui.activity)[RuntimeInfoViewModel::class.java].state.value as RuntimeInfo.Ready
        ui.onNodeWithText("Core ${runtime.core}").assertIsDisplayed()
        ui.onNodeWithText("libghostty-vt ${runtime.terminal}").assertIsDisplayed()
        val documents = mapOf(AboutLicense.GHOSTERM to "Ghosterm-MIT.txt", AboutLicense.GHOSTTY to "Ghostty-MIT.txt",
            AboutLicense.LIBSSH2 to "libssh2-1.11.1.txt", AboutLicense.OPENSSL to "openssl-3.5.4.txt")
        AboutLicense.entries.forEach { license ->
            val tag = "about-license-${license.name}"
            ui.onNodeWithTag(tag).performScrollTo().assertHasClickAction().performClick()
            ui.waitUntil(10_000) { ui.runOnIdle { model().document is AboutDocument.Ready } }
            val bundled = ui.activity.assets.open("licenses/${documents.getValue(license)}").bufferedReader().use { it.readText() }
            ui.onNodeWithTag("about-license-document").assertTextEquals(bundled)
            assertEquals(license, ui.runOnIdle { model().selected })
            ui.activityRule.scenario.recreate()
            ui.onNodeWithTag("about-license-document").assertTextEquals(bundled)
            ui.onNodeWithText("返回").performClick()
            assertNull(ui.runOnIdle { model().selected })
            ui.onNodeWithTag("about-version").performScrollTo().assertIsDisplayed()
        }
        ui.onAllNodesWithTag("about-source").assertCountEquals(1)
        ui.onNodeWithTag("about-source").performScrollTo().assertHasClickAction()
        ui.onNodeWithText("检查更新").assertDoesNotExist()
        ui.onNodeWithText("安装更新").assertDoesNotExist()
        ui.onNodeWithText("返回").performClick()
        assertEquals(Page.SETTINGS, ui.runOnIdle { ViewModelProvider(ui.activity)[HostsViewModel::class.java].page })
    }
}

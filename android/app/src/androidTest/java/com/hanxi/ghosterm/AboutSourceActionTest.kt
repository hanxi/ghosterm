package com.hanxi.ghosterm

import android.app.Application
import android.content.ContextWrapper
import android.content.Intent
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelStore
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AboutSourceActionTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val store = ViewModelStore()
    private lateinit var model: AboutViewModel
    @Before fun setup() {
        ui.runOnIdle {
            model = AboutViewModel(Application()) { """{"schema":1,"git_commit":null,"source_state":"unknown"}""" }
            store.put("about", model)
        }
    }
    @After fun close() { ui.runOnIdle { store.clear() } }
    @Test fun projectSourceButtonDispatchesOnlyTheExplicitProjectIntent() {
        var actual: Intent? = null
        val context = object : ContextWrapper(InstrumentationRegistry.getInstrumentation().targetContext) {
            override fun startActivity(intent: Intent) { actual = intent }
        }
        ui.setContent { CompositionLocalProvider(LocalContext provides context) { MaterialTheme { AboutContent(model, RuntimeInfo.Ready("fixture", "fixture")) } } }
        assertNull(actual)
        ui.onNodeWithTag("about-source").performScrollTo().performClick()
        ui.runOnIdle { assertEquals(Intent.ACTION_VIEW, actual?.action); assertEquals("https://github.com/hanxi/ghosterm", actual?.dataString) }
    }
    @Test fun unavailableBrowserShowsAnErrorAndTheProjectAddress() {
        val context = object : ContextWrapper(InstrumentationRegistry.getInstrumentation().targetContext) {
            override fun startActivity(intent: Intent) { throw android.content.ActivityNotFoundException() }
        }
        ui.setContent { CompositionLocalProvider(LocalContext provides context) { MaterialTheme { AboutContent(model, RuntimeInfo.Ready("fixture", "fixture")) } } }
        ui.onNodeWithTag("about-source").performScrollTo().performClick()
        ui.onNodeWithText("无法打开项目网页，可复制项目地址后打开。").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText(AboutViewModel.SOURCE).performScrollTo().assertIsDisplayed()
    }
}

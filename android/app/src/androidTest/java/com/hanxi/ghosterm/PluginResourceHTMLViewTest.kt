package com.hanxi.ghosterm

import android.graphics.Color
import android.net.Uri
import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import androidx.activity.ComponentActivity
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionResourceStore.Content
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.net.URI

internal fun findPluginHTMLView(view: View): WebView? = (view as? WebView) ?: (view as? ViewGroup)?.let { group ->
    (0 until group.childCount).firstNotNullOfOrNull { findPluginHTMLView(group.getChildAt(it)) }
}
internal fun pluginHTMLPixelCounts(view: WebView): Pair<Int, Int> {
    if (view.width <= 0 || view.height <= 0) return 0 to 0
    val position = IntArray(2).also(view::getLocationOnScreen)
    val bitmap = androidx.test.platform.app.InstrumentationRegistry.getInstrumentation().uiAutomation.takeScreenshot() ?: return 0 to 0
    try {
        var blue = 0; var red = 0
        for (y in position[1].coerceAtLeast(0) until (position[1] + view.height).coerceAtMost(bitmap.height))
            for (x in position[0].coerceAtLeast(0) until (position[0] + view.width).coerceAtMost(bitmap.width)) {
            val c = bitmap.getPixel(x, y)
            if (Color.blue(c) > 240 && Color.red(c) < 16 && Color.green(c) < 16) blue++
            if (Color.red(c) > 240 && Color.blue(c) < 16 && Color.green(c) < 16) red++
        }
        return blue to red
    } finally { bitmap.recycle() }
}
internal class HTMLRequest(private val address: String, private val main: Boolean = false) : WebResourceRequest {
    override fun getUrl() = Uri.parse(address)
    override fun isForMainFrame() = main
    override fun isRedirect() = false
    override fun hasGesture() = false
    override fun getMethod() = "GET"
    override fun getRequestHeaders() = emptyMap<String, String>()
}

@RunWith(AndroidJUnit4::class)
class PluginResourceHTMLViewTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun document() = htmlFixture("docs/index.html", "text/html",
        ("<link rel='stylesheet' href='theme.css'><img src='marker.png'><p>HTML snapshot</p>" +
            "<script>document.body.textContent='SCRIPT_RAN'</script>").toByteArray())
    private fun css(value: String = "body{background:#0000ff!important;min-height:200px}") =
        htmlFixture("docs/theme.css", "text/css", value.toByteArray())
    private fun image() = htmlFixture("docs/marker.png", "image/png", htmlFixturePNG(32, 32))
    private fun webOrNull() = ui.runOnIdle { findPluginHTMLView(ui.activity.window.decorView) }
    private fun web() = checkNotNull(webOrNull())
    private fun awaitRender(): WebView {
        var pixels = 0 to 0
        try {
            ui.waitUntil(15000) {
                pixels = ui.runOnIdle { findPluginHTMLView(ui.activity.window.decorView)?.let(::pluginHTMLPixelCounts) ?: (0 to 0) }
                pixels.first > 10 && pixels.second > 10
            }
        } catch (failure: androidx.compose.ui.test.ComposeTimeoutException) {
            val state = ui.runOnIdle { findPluginHTMLView(ui.activity.window.decorView)?.let {
                "size=${it.width}x${it.height} attached=${it.isAttachedToWindow} shown=${it.isShown} progress=${it.progress} url=${it.url}"
            } ?: "no attached WebView" }
            throw AssertionError("real HTML CSS/PNG pixels missing: $pixels; $state", failure)
        }
        assertTrue("real WebView must draw CSS blue and PNG red: $pixels", pixels.first > 10 && pixels.second > 10)
        return web()
    }
    private fun settle() { repeat(20) { ui.mainClock.advanceTimeByFrame(); SystemClock.sleep(40) } }

    @Suppress("DEPRECATION")
    @Test fun nativeWebViewRendersOnlyFixedCssAndPngWithOfflineConfiguration() {
        val resources = listOf(document(), css(), image())
        ui.setContent { PluginResourcesPanel(resources) }
        val view = awaitRender()
        ui.runOnIdle {
            view.settings.let { settings ->
                assertFalse(settings.javaScriptEnabled); assertFalse(settings.javaScriptCanOpenWindowsAutomatically)
                assertFalse(settings.supportMultipleWindows()); assertFalse(settings.allowFileAccess)
                assertFalse(settings.allowContentAccess); assertFalse(settings.allowFileAccessFromFileURLs)
                assertFalse(settings.allowUniversalAccessFromFileURLs); assertTrue(settings.blockNetworkLoads)
                assertFalse(settings.domStorageEnabled); assertFalse(settings.databaseEnabled)
                assertFalse(settings.safeBrowsingEnabled)
                assertEquals(WebSettings.LOAD_NO_CACHE, settings.cacheMode)
                assertEquals(WebSettings.MIXED_CONTENT_NEVER_ALLOW, settings.mixedContentMode)
            }
            assertFalse(view.isFocusable); assertFalse(view.isFocusableInTouchMode)
            assertEquals(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS, view.importantForAutofill)
            assertFalse(view.isSaveEnabled)
        }
        assertFalse("HTML preview must not summon IME", terminalImeShown())
    }

    @Test fun selectingImageSourceAndRemovingPanelRevokeRetainedClientsAndUseNewOrigins() {
        val d = document(); val i = image(); val resources = mutableStateOf(listOf(d, css(), i))
        ui.setContent { PluginResourcesPanel(resources.value) }
        val first = awaitRender()
        val firstURL = ui.runOnIdle { first.url!! }
        val client = ui.runOnIdle { first.webViewClient }
        val request = HTMLRequest(URI(firstURL).resolve("theme.css").toString())
        assertEquals(200, client.shouldInterceptRequest(first, request)!!.statusCode)
        ui.onNodeWithTag("plugin-resource-html-source-toggle").performClick()
        ui.onNodeWithTag("plugin-resource-html").assertDoesNotExist()
        assertEquals("removed view must revoke future native reads", 403, client.shouldInterceptRequest(first, request)!!.statusCode)
        ui.onNodeWithTag("plugin-resource-html-source-toggle").performClick()
        val second = awaitRender()
        assertNotSame(first, second)
        assertNotEquals(URI(firstURL).host, ui.runOnIdle { URI(second.url!!).host })
        val secondClient = ui.runOnIdle { second.webViewClient }
        val secondRequest = HTMLRequest(ui.runOnIdle { URI(second.url!!).resolve("marker.png").toString() })
        ui.onNodeWithTag("plugin-resource-select-${i.descriptor.resourceId}").performClick()
        ui.onNodeWithTag("plugin-resource-html").assertDoesNotExist()
        assertEquals(403, secondClient.shouldInterceptRequest(second, secondRequest)!!.statusCode)
        ui.onNodeWithTag("plugin-resource-select-${d.descriptor.resourceId}").performClick()
        val third = awaitRender(); val thirdClient = ui.runOnIdle { third.webViewClient }
        val thirdRequest = HTMLRequest(ui.runOnIdle { URI(third.url!!).resolve("marker.png").toString() })
        ui.runOnIdle { resources.value = emptyList() }; ui.waitForIdle()
        assertNull(webOrNull()); assertEquals(403, thirdClient.shouldInterceptRequest(third, thirdRequest)!!.statusCode)
    }

    @Test fun attachmentReplacementRebuildsTheViewAndOldClientCannotReadNewScope() {
        val d = document(); val i = image(); val resources = mutableStateOf(listOf(d, css(), i))
        ui.setContent { PluginResourcesPanel(resources.value) }
        val first = awaitRender(); val client = ui.runOnIdle { first.webViewClient }
        val address = ui.runOnIdle { URI(first.url!!).resolve("theme.css").toString() }
        ui.runOnIdle { resources.value = listOf(d, css("body{background:#0000ff!important}p{color:#00ff00}"), i) }
        ui.waitUntil(15000) { webOrNull()?.let { it !== first } == true }
        awaitRender()
        assertEquals(403, client.shouldInterceptRequest(first, HTMLRequest(address))!!.statusCode)
    }

    @Test fun decomposedDirectoryAndEncodedUnicodeAttachmentsReallyRender() {
        val directory = "docs/cafe\u0301"
        val d = htmlFixture("$directory/页面 #?%.html", "text/html",
            "<link rel='stylesheet' href='%E4%B8%BB%20%E9%A2%98%25.css'><img src='%E5%9B%BE%F0%9F%98%80.png'>".toByteArray())
        val style = htmlFixture("$directory/主 题%.css", "text/css", "body{background:#0000ff!important;min-height:200px}".toByteArray())
        val image = htmlFixture("$directory/图😀.png", "image/png", htmlFixturePNG(32, 32))
        ui.setContent { PluginResourcesPanel(listOf(d, style, image)) }
        val view = awaitRender()
        assertEquals("native URL must not normalize the document path", "/${d.descriptor.displayName}", ui.runOnIdle { URI(view.url!!).path })
    }

    @Test fun documentLinksNeverNavigateOrLaunchAnotherActivity() {
        val d = htmlFixture("docs/index.html", "text/html",
            "<link rel='stylesheet' href='theme.css'><a class='go' href='https://outside.invalid/'>Blocked</a><img src='marker.png'>".toByteArray())
        val style = css("body{background:#0000ff!important}.go{position:fixed;left:0;top:0;width:120px;height:50px;display:block}")
        ui.setContent { PluginResourcesPanel(listOf(d, style, image())) }
        val first = awaitRender(); val url = ui.runOnIdle { first.url }
        ui.onNodeWithTag("plugin-resource-html").performTouchInput { click(Offset(20f, 20f)) }
        settle()
        assertSame("link must keep the original native preview", first, webOrNull())
        assertEquals("link must not navigate", url, ui.runOnIdle { first.url })
        assertTrue(ui.runOnIdle { first.webViewClient.shouldOverrideUrlLoading(first, HTMLRequest("https://outside.invalid/", true)) })
    }
}

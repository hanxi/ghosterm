package com.hanxi.ghosterm

import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.onNodeWithText
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream

@RunWith(AndroidJUnit4::class)
class PluginResourcePreviewUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()

    @Test fun receivedTextIsShownAsSourceAndImageIsRendered() {
        val text = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("a".repeat(64), "text/html", "index.html", 14),
            "<script>x</script>".toByteArray())
        val bitmap = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888).apply { eraseColor(android.graphics.Color.RED) }
        val imageBytes = ByteArrayOutputStream().use { output ->
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)); output.toByteArray()
        }
        bitmap.recycle()
        val image = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("b".repeat(64), "image/png", "image.png", imageBytes.size.toLong()), imageBytes)
        ui.setContent { PluginResourcesPanel(listOf(text, image)) }
        ui.onNodeWithText("<script>x</script>").assertExists()
        ui.onNodeWithText("HTML 按源码显示，不执行脚本或加载网络内容。").assertExists()
        ui.waitUntil(10_000) {
            ui.onAllNodes(androidx.compose.ui.test.hasTestTag("plugin-resource-image")).fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithTag("plugin-resource-image").assertExists()
    }
}

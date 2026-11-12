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
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream

@RunWith(AndroidJUnit4::class)
class PluginResourcePreviewUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()

    @Test fun markdownResourceUsesFormattedPreviewAndExplainsDisabledExternalContent() {
        val markdown = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("c".repeat(64), "text/markdown", "readme.md", 72),
            "# Preview\n\n**formatted** [link](https://example.com) ![image](other.png)".toByteArray())
        ui.setContent { PluginResourcesPanel(listOf(markdown)) }
        ui.waitUntil(10_000) {
            ui.onAllNodesWithTag("plugin-resource-markdown").fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithTag("plugin-resource-markdown").assertExists()
        ui.onNodeWithText("Markdown 链接不打开；图片与外部资源不加载。", substring = true).assertExists()
        ui.onNodeWithText("[图片: image]", substring = true).assertExists()
    }

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

    @Test fun launchDialogPreservesAnExplicitWorkspaceRootInTheSSHRequest() {
        val digest = "a".repeat(64)
        val command = ExtensionController.Command("preview", "Preview", "{}", listOf("resource"))
        val review = ExtensionController.Review("org.ghosterm.preview", "0.1.0", "Ghosterm", "MIT",
            digest, "portable-js", listOf("resource.receive"), emptyList(), true, false, listOf(command))
        var captured: ExtensionRelayBootstrap.Request? = null
        ui.setContent {
            PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), command,
                listOf("host"), onDismiss = {}, onLaunch = { _, request -> captured = request })
        }
        val root = "/home/user's space/工作区"
        ui.onNodeWithTag("plugin-executor-directory").performTextInput("/opt/preview")
        ui.onNodeWithTag("plugin-executor-digest").performTextInput("b".repeat(64))
        ui.onNodeWithTag("plugin-workspace-root").performTextInput(root)
        ui.onNodeWithText("连接并请求授权").performClick()
        assertEquals(root, captured?.workspaceRootDirectory)
        assertTrue(ExtensionRelayBootstrap.build(checkNotNull(captured)).contains("--root' 'workspace'"))
        assertTrue(ExtensionRelayBootstrap.build(checkNotNull(captured)).contains("'/home/user'\\''s space/工作区'"))
    }

    @Test fun consentDialogShowsTheExactRemoteCommandAndCapabilitySet() {
        val command = ExtensionController.Command("preview", "Preview", "{}", listOf("resource"))
        val info = com.hanxi.ghosterm.bridge.ExtensionSession.Info(
            com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT,
            com.hanxi.ghosterm.bridge.ExtensionSession.End.NONE, 0, 0,
            com.hanxi.ghosterm.bridge.ExtensionSession.Decision.UNKNOWN, "connection", "instance", "token",
            "0.1.0", 1, 1000, 1, 1, com.hanxi.ghosterm.bridge.ExtensionSession.Budgets(),
            com.hanxi.ghosterm.bridge.ExtensionSession.PeerKind.SSH, "preview-host")
        val active = ExtensionPackagesViewModel.ActiveSession("org.ghosterm.preview", "a".repeat(64), command,
            listOf("fixture.txt"), "{\"path\":\"fixture.txt\"}", emptyList(), emptyList(), "b".repeat(64),
            "preview-host", "exec 'gterm' '--root' 'workspace' '/safe/root'", listOf("resource.receive", "file.readScoped"), info)
        var decision: Boolean? = null
        ui.setContent { PluginConsentDialog(active, busy = false, onDecision = { decision = it }) }
        ui.onNodeWithTag("plugin-consent-remote-command").assertTextEquals(active.remoteCommand)
        ui.onNodeWithText("需要能力：resource.receive, file.readScoped").assertExists()
        ui.onNodeWithText("允许并启动").performClick()
        assertEquals(true, decision)
    }
}

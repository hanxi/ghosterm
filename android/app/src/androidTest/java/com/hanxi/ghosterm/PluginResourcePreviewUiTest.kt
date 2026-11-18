package com.hanxi.ghosterm

import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.*
import androidx.compose.ui.graphics.asAndroidBitmap
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

    @Test fun htmlReceiptUsesNativePreviewAndKeepsSourceAnExplicitChoice() {
        val bytes = "<h1>HTML Preview</h1><script>document.body.textContent='must not run'</script>".toByteArray()
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(bytes)
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        val resource = ExtensionResourceStore.Content(ExtensionResourceStore.Descriptor(
            sha, "text/html", "docs/index.html", bytes.size.toLong(), resourceId = "html-document"), bytes)
        ui.setContent { PluginResourcesPanel(listOf(resource)) }
        ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-resource-html").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("plugin-resource-html").assertExists()
        ui.onNodeWithText(bytes.toString(Charsets.UTF_8)).assertDoesNotExist()
        ui.onNodeWithTag("plugin-resource-html-source-toggle").performClick()
        ui.onNodeWithText(bytes.toString(Charsets.UTF_8)).assertExists()
    }

    @Test fun markdownRendersOnlyAnExplicitSameDirectoryRasterSnapshot() {
        val bitmap = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888).apply { eraseColor(android.graphics.Color.RED) }
        val imageBytes = ByteArrayOutputStream().use { output ->
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)); output.toByteArray()
        }
        bitmap.recycle()
        val markdown = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("d".repeat(64), "text/markdown", "docs/readme.md", 35,
                resourceId = "markdown-resource"),
            "![diagram](diagram.png) ![outside](../outside.png) ![remote](https://example.com/a.png)".toByteArray())
        val image = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("e".repeat(64), "image/png", "docs/diagram.png", imageBytes.size.toLong(),
                resourceId = "diagram-resource"), imageBytes)
        ui.setContent { PluginResourcesPanel(listOf(markdown, image)) }
        ui.waitUntil(10_000) {
            ui.onAllNodesWithTag("plugin-resource-markdown-inline-image").fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithTag("plugin-resource-markdown-inline-image").assertExists()
        val preview = ui.onNodeWithTag("plugin-resource-markdown").captureToImage().asAndroidBitmap()
        var redPixels = 0
        for (y in 0 until preview.height) for (x in 0 until preview.width) {
            val color = preview.getPixel(x, y)
            if (android.graphics.Color.red(color) > 240 && android.graphics.Color.green(color) < 32 &&
                android.graphics.Color.blue(color) < 32) redPixels++
        }
        assertTrue("expected Markdown attachment pixels, got $redPixels", redPixels > 0)
        ui.onNodeWithText("[图片: outside]", substring = true).assertExists()
        ui.onNodeWithText("[图片: remote]", substring = true).assertExists()
    }

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
        val html = "<script>x</script>".toByteArray()
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(html)
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        val text = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor(sha, "text/html", "index.html", html.size.toLong(), resourceId = "html-resource"), html)
        val bitmap = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888).apply { eraseColor(android.graphics.Color.RED) }
        val imageBytes = ByteArrayOutputStream().use { output ->
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)); output.toByteArray()
        }
        bitmap.recycle()
        val image = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("b".repeat(64), "image/png", "image.png", imageBytes.size.toLong(),
                resourceId = "image-resource"), imageBytes)
        ui.setContent { PluginResourcesPanel(listOf(text, image)) }
        ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-resource-html").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("plugin-resource-html-source-toggle").performClick()
        ui.onNodeWithText("<script>x</script>").assertExists()
        ui.onNodeWithText("仅加载显式附件；脚本、外链和表单提交不启用。").assertExists()
        ui.onNodeWithTag("plugin-resource-select-${image.descriptor.resourceId}").performClick()
        ui.waitUntil(10_000) {
            ui.onAllNodes(androidx.compose.ui.test.hasTestTag("plugin-resource-image")).fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithTag("plugin-resource-image").assertExists()
    }

    @Test fun selectingAResourceShowsOnlyItsPreviewAndFallsBackAfterRemoval() {
        val first = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("1".repeat(64), "text/plain", "first.txt", 5,
                resourceId = "first-resource"), "first".toByteArray())
        val second = ExtensionResourceStore.Content(
            ExtensionResourceStore.Descriptor("2".repeat(64), "text/plain", "second.txt", 6,
                resourceId = "second-resource"), "second".toByteArray())
        val resources = androidx.compose.runtime.mutableStateOf(listOf(first, second))
        ui.setContent { PluginResourcesPanel(resources.value) }

        ui.onNodeWithTag("plugin-resource-${first.descriptor.resourceId}").assertExists()
        ui.onNodeWithText("first").assertExists()
        ui.onNodeWithTag("plugin-resource-${second.descriptor.resourceId}").assertDoesNotExist()

        ui.onNodeWithTag("plugin-resource-select-${second.descriptor.resourceId}").performClick()
        ui.onNodeWithTag("plugin-resource-${second.descriptor.resourceId}").assertExists()
        ui.onNodeWithText("second").assertExists()
        ui.onNodeWithTag("plugin-resource-${first.descriptor.resourceId}").assertDoesNotExist()

        ui.runOnUiThread { resources.value = listOf(first) }
        ui.waitUntil(10_000) {
            ui.onAllNodesWithTag("plugin-resource-${first.descriptor.resourceId}").fetchSemanticsNodes().isNotEmpty()
        }
        ui.onNodeWithText("first").assertExists()
        ui.onNodeWithTag("plugin-resource-${second.descriptor.resourceId}").assertDoesNotExist()
    }

    @Test fun launchDialogPreservesAnExplicitWorkspaceRootInTheSSHRequest() {
        val digest = "a".repeat(64)
        val command = ExtensionController.Command("preview", "Preview", "{}", listOf("resource"))
        val review = ExtensionController.Review("org.ghosterm.preview", "0.1.0", "Ghosterm", "MIT",
            digest, "portable-js", listOf("resource.receive"), emptyList(), true, false, listOf(command))
        var captured: ExtensionRelayBootstrap.Request? = null
        ui.setContent {
            PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), command,
                listOf("host"), onDismiss = {}, onLaunch = { _, request, _ -> captured = request })
        }
        val root = "/home/user's space/工作区"
        ui.onNodeWithTag("plugin-executor-directory").performTextInput("/opt/preview")
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextInput("b".repeat(64))
        ui.onNodeWithTag("plugin-workspace-root").performScrollTo().performTextInput(root)
        ui.onNodeWithText("连接并请求授权").performClick()
        assertEquals(root, captured?.workspaceRootDirectory)
        assertTrue(ExtensionRelayBootstrap.build(checkNotNull(captured)).contains("--root' 'workspace'"))
        assertTrue(ExtensionRelayBootstrap.build(checkNotNull(captured)).contains("'/home/user'\\''s space/工作区'"))
    }

    @Test fun rejectedGroupPreservesInputsAndAttachmentUsesItsOwnSubmitAction() {
        val command = ExtensionController.Command("preview", "Preview", "{}", emptyList())
        val review = ExtensionController.Review("org.ghosterm.preview", "0.1.0", "Ghosterm", "MIT",
            "a".repeat(64), "portable-js", emptyList(), emptyList(), true, false, listOf(command))
        val rejection = "同一连接组必须选择同一目标"
        ui.setContent {
            PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), command,
                listOf("host"), onDismiss = {}, onLaunch = { _, _, _ -> error("未提交单独启动") },
                onEnqueue = { _, _, _, _ -> Result.failure(IllegalArgumentException(rejection)) })
        }
        val directory = ui.onNodeWithTag("plugin-executor-directory")
        directory.performScrollTo().performTextInput("/opt/preview")
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextInput("b".repeat(64))
        ui.onNodeWithTag("plugin-group-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-group-temporary").performScrollTo().performTextInput("/opt/tmp")
        ui.onNodeWithText("加入待启动组").performClick()
        ui.onNodeWithText(rejection).assertExists()
        directory.assertTextContains("/opt/preview")

        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithText("加入待启动组").assertDoesNotExist()
        ui.onNodeWithText("连接并请求授权").assertExists()
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

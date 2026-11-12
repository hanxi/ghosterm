package com.hanxi.ghosterm

import android.os.SystemClock
import android.os.Bundle
import android.util.Base64
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.view.accessibility.AccessibilityNodeInfo
import androidx.lifecycle.ViewModelProvider
import androidx.compose.ui.test.*
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.PreparedSSH
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest

/** Real Settings page, production ExtensionPackagesViewModel and two-hop SSH fixture. */
@RunWith(AndroidJUnit4::class)
class PluginPreviewSettingsE2ETest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()

    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        java.net.Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            val stream = java.io.DataInputStream(socket.getInputStream())
            val size = stream.readInt()
            check(size in 1..1_048_576)
            ByteArray(size).also(stream::readFully).toString(Charsets.UTF_8).let(::JSONObject)
        }
    }

    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST")
        return result as T
    }

    private fun await(label: String, predicate: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 30_000
        while (SystemClock.elapsedRealtime() < deadline) {
            if (main(predicate)) return
            SystemClock.sleep(20)
        }
        fail(label)
    }

    private fun systemNodes(): List<AccessibilityNodeInfo> {
        val result = mutableListOf<AccessibilityNodeInfo>()
        fun visit(node: AccessibilityNodeInfo?) {
            if (node == null) return
            result += node
            for (index in 0 until node.childCount) visit(node.getChild(index))
        }
        visit(instrumentation.uiAutomation.rootInActiveWindow)
        return result
    }

    private fun clickSystem(vararg labels: String): Boolean {
        for (match in systemNodes().filter { it.isVisibleToUser &&
            (it.text?.toString() in labels || it.contentDescription?.toString() in labels) }) {
            when (tapLegacyDocumentRow(instrumentation, match)) {
                true -> return true
                false -> continue
                null -> Unit
            }
            var node: AccessibilityNodeInfo? = match
            while (node != null) {
                if (node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
                node = node.parent
            }
        }
        return false
    }

    private fun awaitSystem(label: String, predicate: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (SystemClock.elapsedRealtime() < deadline) {
            if (predicate()) return
            SystemClock.sleep(40)
        }
        fail("$label: ${systemNodes().take(35).joinToString { "${it.text}/${it.contentDescription}" }}")
    }

    @Test fun realTwoHopPreviewReceiptAppearsInMainActivitySettings() {
        assumeTrue("requires the explicit JS-enabled Android build", BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture
        val ports = payload.getJSONObject("two").getJSONArray("ports")
        val packageFiles = payload.getJSONObject("previewPackageFiles")
        val names = packageFiles.keys()
        val providerFiles = JSONObject()
        while (names.hasNext()) {
            val name = names.next()
            providerFiles.put(name, packageFiles.getString(name))
        }
        val digest = MessageDigest.getInstance("SHA-256").digest(
            Base64.decode(packageFiles.getString("integrity.json"), Base64.DEFAULT))
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals(payload.getString("previewPackageDigest"), digest)

        val targetContext = instrumentation.targetContext
        val treeUri = DocumentsContract.buildTreeDocumentUri(PreviewPackageDocumentsProvider.AUTHORITY, "preview-root")
        val providerUri = Uri.parse("content://com.hanxi.ghosterm.test.preview-package-control")
        val grantFlags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
        val installResult = targetContext.contentResolver.call(providerUri, "install", null,
            Bundle().apply { putString("files", providerFiles.toString()) })
        assertTrue(installResult?.getString("error") ?: "SAF package fixture install failed",
            installResult?.getBoolean("ok") == true)

        val knownHosts = File(instrumentation.targetContext.filesDir, "home/.ssh/known_hosts")
        check(knownHosts.parentFile!!.isDirectory || knownHosts.parentFile!!.mkdirs())
        val originalKnownHosts = knownHosts.takeIf(File::exists)?.readBytes()
        val user = payload.getString("user")
        val target = PreparedSSH("dest", "127.0.0.1", user, ports.getInt(1), emptyList(),
            listOf(PreparedSSH("jump", "127.0.0.1", user, ports.getInt(0), emptyList())))
        val activity = ui.activity
        val extensions = ViewModelProvider(activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(activity)[SessionsViewModel::class.java]
        val controller = checkNotNull(extensions.controller)
        try {
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithText("选择插件文件夹").performScrollTo().performClick()
            awaitSystem("DocumentsUI did not open") {
                systemNodes().any { it.packageName?.toString()?.contains("documentsui") == true }
            }
            awaitSystem("DocumentsUI roots did not open") {
                clickSystem("Show roots", "Open navigation drawer", "显示根目录", "显示位置")
            }
            awaitSystem("Preview package provider did not open") {
                clickSystem("Ghosterm Official Preview Package Fixture")
            }
            awaitSystem("SAF folder selection was not accepted") {
                clickSystem("Use this folder", "USE THIS FOLDER", "使用此文件夹",
                    "ALLOW ACCESS TO \"GHOSTERM OFFICIAL PREVIEW PACKAGE FIXTURE\"")
            }
            awaitSystem("SAF read permission was not accepted") {
                clickSystem("Allow", "ALLOW", "允许") ||
                    systemNodes().none { it.packageName?.toString()?.contains("documentsui") == true }
            }
            awaitSystem("DocumentsUI did not return to the product Settings page") {
                systemNodes().none { it.packageName?.toString()?.contains("documentsui") == true }
            }
            await("Settings page did not review the SAF package") {
                !extensions.busy && extensions.entries.any { it.review.id == "org.ghosterm.preview" }
            }
            val pending = main { extensions.entries.single { it.review.id == "org.ghosterm.preview" } }
            assertEquals(digest, pending.review.digest)
            ui.onNodeWithText("批准此摘要").performScrollTo().performClick()
            await("Settings page did not approve the reviewed package") {
                !extensions.busy &&
                    extensions.entries.singleOrNull { it.review.id == "org.ghosterm.preview" }?.decision == "已批准（本进程）"
            }
            val entry = main { extensions.entries.single { it.review.id == "org.ghosterm.preview" } }
            val review = entry.review
            val command = review.commands.single { it.path == "preview" }
            val request = ExtensionRelayBootstrap.Request(
                executable = payload.getString("previewGtermWrapper"), executorDirectory = payload.getString("previewPackageDirectory"),
                executorDigest = digest, clientDigest = digest, methods = emptyList(), command = "preview",
                arguments = listOf("docs/preview.md", "--assets", "docs/marker.png"),
                workspaceRootDirectory = payload.getString("previewWorkspace"))

            System.out.println("Preview launch id=${entry.review.id} digestMatches=${entry.review.digest == request.clientDigest} " +
                "decision=${entry.decision} command=${command.path} args=${request.arguments.size}:${request.arguments.firstOrNull()}")
            main { extensions.launch(entry, command, target, request, sessions) }
            val authDeadline = SystemClock.elapsedRealtime() + 90_000
            var handledPrompt: Triple<Long, Int, SSHAuthPhase>? = null
            while (main { extensions.activeSession == null && extensions.error == null } &&
                SystemClock.elapsedRealtime() < authDeadline) {
                val attempt = main { sessions.connecting }
                val prompt = attempt?.auth
                val currentPrompt = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (currentPrompt == null || currentPrompt == handledPrompt) {
                    SystemClock.sleep(20)
                    continue
                }
                handledPrompt = currentPrompt
                System.out.println("Preview Settings SSH auth phase=${prompt?.phase} hop=${prompt?.hop} request=${prompt?.request}")
                when (prompt?.phase) {
                    SSHAuthPhase.HOSTKEY -> sessions.decide(checkNotNull(attempt).id, checkNotNull(prompt), true)
                    SSHAuthPhase.CREDENTIALS -> runBlocking {
                        controller.submitSSHPrivateKey(
                            checkNotNull(prompt), payload.getString("pem").toByteArray(), byteArrayOf())
                    }
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("SSH plugin authentication failed: ${prompt.description}")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNotNull("SSH route was not adopted: attempt=${main { sessions.connecting?.message }} " +
                "error=${main { extensions.error }}", main { extensions.activeSession })
            assertEquals("{\"assets\":\"docs/marker.png\",\"maxBytes\":262144,\"path\":\"docs/preview.md\"}",
                main { extensions.activeSession!!.decodedArguments })
            await("Settings page did not reach Session consent: session=${main { extensions.activeSession?.info }} " +
                "error=${main { extensions.error }}") {
                extensions.activeSession?.info?.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT
            }
            ui.onNodeWithTag("plugin-consent-remote-command")
                .assertTextEquals(extensions.activeSession!!.remoteCommand)
            ui.onNodeWithText("需要能力：resource.receive, file.readScoped").assertExists()
            ui.onNodeWithText("允许并启动").performClick()

            val resourceDeadline = SystemClock.elapsedRealtime() + 60_000
            while (extensions.receivedResources.none { it.descriptor.displayName == "docs/preview.md" } &&
                SystemClock.elapsedRealtime() < resourceDeadline) SystemClock.sleep(20)
            assertTrue("MainActivity Settings did not receive Preview resource: " +
                "error=${main { extensions.error }} runtime=${main { extensions.activeRuntime }} " +
                "session=${main { extensions.activeSession?.info }}",
                extensions.receivedResources.any { it.descriptor.displayName == "docs/preview.md" })
            await("Settings did not receive both the Markdown document and its explicit image attachment") {
                extensions.receivedResources.map { it.descriptor.displayName }.toSet() ==
                    setOf("docs/preview.md", "docs/marker.png")
            }
            val received = extensions.receivedResources.single { it.descriptor.displayName == "docs/preview.md" }
            val expectedMarkdown = "# Android Preview\n\n![red marker](marker.png)\n\nAndroid Markdown over two-hop SSH\n"
            assertArrayEquals(expectedMarkdown.toByteArray(), received.bytes)
            assertEquals("text/markdown", received.descriptor.mime)
            val image = extensions.receivedResources.single { it.descriptor.displayName == "docs/marker.png" }
            assertEquals("image/png", image.descriptor.mime)
            ui.onNodeWithTag("plugin-runtime-state").assertExists()
            ui.onNodeWithTag("plugin-resource-${received.descriptor.resourceId}").performScrollTo().assertExists()
            ui.onNodeWithTag("plugin-resource-markdown").assertExists()
            ui.waitUntil(10_000) {
                ui.onAllNodesWithTag("plugin-resource-markdown-inline-image").fetchSemanticsNodes().isNotEmpty()
            }
            ui.onNodeWithTag("plugin-resource-markdown-inline-image").assertExists()
            ui.onNodeWithText("Android Markdown over two-hop SSH", substring = true).assertExists()
            val preview = ui.onNodeWithTag("plugin-resource-markdown").captureToImage().asAndroidBitmap()
            var redPixels = 0
            for (y in 0 until preview.height) for (x in 0 until preview.width) {
                val color = preview.getPixel(x, y)
                if (android.graphics.Color.red(color) > 240 && android.graphics.Color.green(color) < 32 &&
                    android.graphics.Color.blue(color) < 32) redPixels++
            }
            assertTrue("real SSH Markdown attachment should draw red pixels; got $redPixels", redPixels > 0)

            ui.onNodeWithText("停止插件").performScrollTo().performClick()
            await("Settings stop action did not stop the Client Runtime: ${main { extensions.activeRuntime }}") {
                extensions.activeRuntime?.info?.phase == com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.STOPPED
            }
            ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
            await("Settings release action did not clear the Session, Runtime, and resource receipt: " +
                "runtime=${main { extensions.activeRuntime }} session=${main { extensions.activeSession }} " +
                "resources=${main { extensions.receivedResources.size }} error=${main { extensions.error }}") {
                extensions.activeRuntime == null && extensions.activeSession == null &&
                    extensions.receivedResources.isEmpty() && !extensions.launchBusy
            }
            assertNull("stopping and releasing the received Preview must not leave an AppModel error",
                extensions.error)
        } finally {
            var closeFailure: Throwable? = null
            try { runBlocking { controller.close() } } catch (failure: Throwable) { closeFailure = failure }
            if (originalKnownHosts == null) knownHosts.delete() else knownHosts.writeBytes(originalKnownHosts)
            targetContext.contentResolver.call(providerUri, "cleanup", null, Bundle().apply {
                putString("uri", treeUri.toString()); putInt("flags", grantFlags)
            })
            closeFailure?.let { throw it }
        }
    }
}

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
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest

/** Real Settings page, production ExtensionPackagesViewModel and direct or two-hop SSH fixtures. */
@RunWith(AndroidJUnit4::class)
class PluginPreviewSettingsE2ETest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()

    private val fixture: JSONObject by lazy { fixtureMessage() }
    private fun fixtureMessage(command: String = ""): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        return java.net.Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + command + "\n").toByteArray())
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

    @Test fun realTwoHopPreviewReceiptAppearsInMainActivitySettings() = preview(null)
    @Test fun realTwoHopPreviewReturnsToSamePtyWithOpenIme() = preview(true)
    @Test fun realTwoHopPreviewReturnsToSamePtyWithoutSummoningHiddenIme() = preview(false)
    @Test fun realTwoHopHtmlAppearsInMainActivitySettings() = preview(null, html = true)
    @Test fun realTwoHopHtmlReturnsToSamePtyWithOpenIme() = preview(true, html = true)
    @Test fun realTwoHopHtmlReturnsToSamePtyWithoutSummoningHiddenIme() = preview(false, html = true)
    @Test fun realTwoHopUnicodeHtmlAppearsInMainActivitySettings() = preview(null, html = true, unicode = true)
    @Test fun realDirectPreviewReceiptAppearsInMainActivitySettings() = preview(null, hopCount = 1)
    @Test fun realDirectHtmlAppearsInMainActivitySettings() = preview(null, html = true, hopCount = 1)
    @Test fun realDirectUnicodeHtmlAppearsInMainActivitySettings() = preview(null, html = true, unicode = true, hopCount = 1)
    @Test fun luaClientPublicationUsesApprovedWorkspaceAndRuntime() =
        preview(null, hopCount = 1, clientPublication = true)
    @Test fun luaClientPublicationRejectsLinkedWorkspace() =
        preview(null, hopCount = 1, clientPublication = true, linkedWorkspace = true)
    @Test fun luaDenialDoesNotActivateOnDirectSSH() = preview(null, hopCount = 1, denyConsent = true)
    @Test fun luaDenialDoesNotActivateOnProxyJump() = preview(null, denyConsent = true)
    @Test fun luaTmuxForegroundAttachmentOverDirectSSH() = preview(null, hopCount = 1, tmuxForeground = true)
    @Test fun luaTmuxForegroundAttachmentOverProxyJump() = preview(null, tmuxForeground = true)

    private fun preview(returnKeyboard: Boolean?, html: Boolean = false, unicode: Boolean = false, hopCount: Int = 2,
                        clientPublication: Boolean = false, linkedWorkspace: Boolean = false, denyConsent: Boolean = false,
                        tmuxForeground: Boolean = false) {
        org.junit.Assume.assumeTrue("requires the owned Preview fixture",
            InstrumentationRegistry.getArguments().containsKey("fixturePort"))
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        val payload = fixture
        assertEquals("fixture format must match the requested scenario", if (unicode) "html-unicode" else if (html) "html" else "markdown", payload.getString("previewFormat"))
        val routeLabel = if (hopCount == 1) "direct" else "two-hop"
        val directory = if (unicode) "docs/cafe\u0301" else "docs"
        val documentPath = if (unicode) "$directory/页面 #?%.html" else if (html) "docs/index.html" else "docs/preview.md"
        val cssPath = if (unicode) "$directory/主 题%.css" else "docs/theme.css"
        val imagePath = if (unicode) "$directory/图😀.png" else "docs/marker.png"
        val assetPaths = if (html) "$cssPath,$imagePath" else imagePath
        val cssLink = if (unicode) "%E4%B8%BB%20%E9%A2%98%25.css" else "theme.css"
        val imageLink = if (unicode) "%E5%9B%BE%F0%9F%98%80.png" else "marker.png"
        val expectedDocument = if (html) "<!doctype html><link rel='stylesheet' href='$cssLink'><img src='$imageLink'>" +
            "<p>Android HTML over $routeLabel SSH</p><script>document.body.textContent='SCRIPT_RAN'</script>"
            else "# Android Preview\n\n![red marker](marker.png)\n\nAndroid Markdown over $routeLabel SSH\n"
        var retainedView: android.webkit.WebView? = null
        var retainedClient: android.webkit.WebViewClient? = null
        var retainedRequest: HTMLRequest? = null
        fun captureHTML() {
            ui.waitUntil(10_000) { ui.onAllNodesWithTag("plugin-resource-html").fetchSemanticsNodes().isNotEmpty() }
            ui.onNodeWithTag("plugin-resource-html").performScrollTo()
            var pixels = 0 to 0
            ui.waitUntil(15_000) {
                pixels = main { findPluginHTMLView(ui.activity.window.decorView)?.let(::pluginHTMLPixelCounts) ?: (0 to 0) }
                pixels.first > 10 && pixels.second > 10
            }
            assertTrue("real SSH HTML must draw fixed CSS blue and PNG red: $pixels", pixels.first > 10 && pixels.second > 10)
            retainedView = main { findPluginHTMLView(ui.activity.window.decorView) }
            assertEquals("received document URL must preserve exact path code points", "/$documentPath",
                main { java.net.URI(retainedView!!.url!!).path })
            retainedClient = main { retainedView!!.webViewClient }
            retainedRequest = HTMLRequest(main { java.net.URI(retainedView!!.url!!).resolve(android.net.Uri.encode(cssPath.substringAfterLast('/'))).toString() })
        }
        val route = payload.getJSONObject("route")
        val ports = route.getJSONArray("ports")
        assertEquals("fixture route must match the requested scenario", hopCount, ports.length())
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
        val jumps = (0 until hopCount - 1).map { PreparedSSH("jump", "127.0.0.1", user, ports.getInt(it), emptyList()) }
        val target = PreparedSSH("dest", "127.0.0.1", user, ports.getInt(hopCount - 1), emptyList(), jumps)
        val activity = ui.activity
        val extensions = ViewModelProvider(activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(activity)[SessionsViewModel::class.java]
        val controller = checkNotNull(extensions.controller)
        val terminalProbe = PluginTerminalReturnProbe(ui)
        val clientRoot = if (clientPublication) File(targetContext.cacheDir, "client-" + java.util.UUID.randomUUID()) else null
        val clientFile = clientRoot?.resolve("client.txt")
        val linkedRoot = clientRoot?.takeIf { linkedWorkspace }?.let { it.resolveSibling(it.name + "-link") }
        try {
            clientRoot?.let {
                check(it.mkdir())
                clientFile!!.writeText("Client publication over Lua SSH")
            }
            if (linkedRoot != null) android.system.Os.symlink(checkNotNull(clientRoot).absolutePath, linkedRoot.absolutePath)
            val originalTerminal = returnKeyboard?.let {
                terminalProbe.begin().also { terminalProbe.leaveToPlugins(returnKeyboard) }
            }
            if (returnKeyboard == null) {
                ui.onNodeWithContentDescription("设置").performClick()
                ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            }
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
            if (returnKeyboard == false && originalTerminal != null) {
                terminalProbe.returnFromPlugins(originalTerminal, false)
                ui.onNodeWithTag("terminal-menu").performClick()
                ui.onNodeWithTag("terminal-plugin-commands").performScrollTo().performClick()
                ui.onNodeWithTag("terminal-plugin-command-preview").performClick()
                ui.onNodeWithContentDescription("取消插件启动").performClick()
                ui.onNodeWithText("关闭").performClick()
                terminalProbe.assertReturned(originalTerminal, false)
                terminalProbe.leaveToPlugins(false)
            }
            val entry = main { extensions.entries.single { it.review.id == "org.ghosterm.preview" } }
            val review = entry.review
            val command = review.commands.single { it.path == "preview" }
            val attachment = if (tmuxForeground) ExtensionRelayBootstrap.Attachment(
                payload.getString("foregroundDirectory"), payload.getString("foregroundInstance"),
                authorized = true, scopedFiles = true) else null
            val request = ExtensionRelayBootstrap.Request(
                executable = payload.getString("previewGtermWrapper"),
                executorDirectory = if (attachment == null) payload.getString("previewPackageDirectory") else "",
                executorDigest = digest, clientDigest = digest, methods = emptyList(), command = "preview",
                arguments = listOf(documentPath, "--assets", assetPaths),
                workspaceRootDirectory = if (attachment == null) payload.getString("previewWorkspace") else null,
                attachment = attachment)

            System.out.println("Preview launch id=${entry.review.id} digestMatches=${entry.review.digest == request.clientDigest} " +
                "decision=${entry.decision} command=${command.path} args=${request.arguments.size}:${request.arguments.firstOrNull()}")
            val clientWorkspace = (linkedRoot ?: clientRoot)?.let {
                if (!linkedWorkspace) {
                    val unapproved = ExtensionController.ClientWorkspace(it.absolutePath, authorized = false)
                    assertTrue("Lua Client publication requires a workspace grant",
                        runCatching { unapproved.validate(review.requiredCapabilities) }.exceptionOrNull() is IllegalArgumentException)
                }
                ExtensionController.ClientWorkspace(it.absolutePath, authorized = true)
            }
            main { extensions.launch(entry, command, target, request, sessions, clientWorkspace) }
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
            assertEquals("{\"assets\":\"$assetPaths\",\"maxBytes\":262144,\"path\":\"$documentPath\"}",
                main { extensions.activeSession!!.decodedArguments })
            val invocation = main { extensions.activeSession!!.invocationContext }
            if (originalTerminal != null) {
                assertEquals(originalTerminal.id.toString(), invocation?.workspaceID)
                assertEquals(originalTerminal.id.toString(), invocation?.paneID)
            }
            assertNull(invocation?.directory)
            await("Settings page did not reach Session consent: session=${main { extensions.activeSession?.info }} " +
                "error=${main { extensions.error }}") {
                extensions.activeSession?.info?.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT
            }
            ui.onNodeWithTag("plugin-consent-remote-command")
                .assertTextEquals(extensions.activeSession!!.remoteCommand)
            if (attachment != null) {
                assertEquals(attachment, main { extensions.activeSession?.executorAttachment })
                assertTrue(main { extensions.activeSession!!.remoteCommand.contains("'--attach'") })
            }
            val required = main { extensions.activeSession!!.requiredCapabilities }
            assertEquals(setOf("session.rpc", "resource.receive", "file.readScoped"), required.toSet())
            ui.onNodeWithText("需要能力：${required.joinToString()}").assertExists()
            if (denyConsent) {
                ui.onNodeWithTag("plugin-consent-deny").performClick()
                await("Refused Lua Session did not end") {
                    val phase = extensions.activeSession?.info?.phase
                    phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.ENDED ||
                        phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CLOSED
                }
                assertNull("Refused Lua Client must never activate", main { extensions.activeRuntime })
                assertTrue(extensions.receivedResources.isEmpty())
                assertNull(extensions.error)
                ui.onNodeWithText("释放插件 Session").performScrollTo().performClick()
                await("Refused Lua Session was not released") { extensions.activeSession == null }
                assertNull(extensions.error)
                return
            }
            ui.onNodeWithText("允许并启动").performClick()
            if (linkedWorkspace) {
                await("Client workspace validation produced no outcome") {
                    extensions.error != null || extensions.activeRuntime != null
                }
                assertNotNull("Lua Client publication must reject a linked workspace", main { extensions.error })
                return
            }

            val resourceDeadline = SystemClock.elapsedRealtime() + 60_000
            while (extensions.receivedResources.none { it.descriptor.displayName == documentPath } &&
                SystemClock.elapsedRealtime() < resourceDeadline) SystemClock.sleep(20)
            assertTrue("MainActivity Settings did not receive Preview resource: " +
                "error=${main { extensions.error }} runtime=${main { extensions.activeRuntime }} " +
                "session=${main { extensions.activeSession?.info }}",
                extensions.receivedResources.any { it.descriptor.displayName == documentPath })
            await("Settings did not receive the exact document and explicit attachments") {
                extensions.receivedResources.map { it.descriptor.displayName }.toSet() ==
                    (setOf(documentPath, imagePath) + if (html) setOf(cssPath) else emptySet())
            }
            val received = extensions.receivedResources.single { it.descriptor.displayName == documentPath }
            assertArrayEquals(expectedDocument.toByteArray(), received.bytes)
            assertEquals(if (html) "text/html" else "text/markdown", received.descriptor.mime)
            val image = extensions.receivedResources.single { it.descriptor.displayName == imagePath }
            assertEquals("image/png", image.descriptor.mime)
            assertEquals("received PNG byte count", payload.getInt("previewImageBytes"), image.bytes.size)
            val imageDigest = MessageDigest.getInstance("SHA-256").digest(image.bytes)
                .joinToString("") { "%02x".format(it.toInt() and 255) }
            assertEquals("received PNG digest", payload.getString("previewImageSHA256"), imageDigest)
            if (payload.optBoolean("killActivePane")) {
                val pending = runBlocking {
                    controller.requestRuntimeExecutor("preview.open", """{"path":"pending","assets":"","maxBytes":3}""".toByteArray())
                }
                val handle = checkNotNull(pending.handle)
                await("Lua executor did not publish pending readiness") {
                    extensions.receivedResources.any { it.descriptor.displayName == "pending.bin" }
                }
                assertTrue(fixtureMessage(" kill-active").getBoolean("closed"))
                await("Disconnected Preview Runtime did not fail") {
                    extensions.activeRuntime?.info?.phase == com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.FAILED
                }
                val result = runBlocking { controller.runtimeExecutorRequestResult(handle) }.second
                assertEquals(com.hanxi.ghosterm.bridge.ExtensionSession.RPCState.UNKNOWN, result?.state)
                assertFalse(checkNotNull(result).confirmed)
                runBlocking { controller.forgetRuntimeExecutorRequest(handle) }
                ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
                await("Disconnected Preview owner was not released") { extensions.activeRuntime == null && extensions.activeSession == null }
                return
            }
            if (clientPublication) {
                val active = checkNotNull(main { extensions.activeRuntime })
                val path = checkNotNull(clientFile).name
                val stale = runBlocking {
                    runCatching { controller.publishClientResource(java.util.UUID.randomUUID().toString(), path) }
                }
                assertTrue("Lua Client publication must reject a stale Runtime identity", stale.exceptionOrNull() is IllegalStateException)
                val publication = runBlocking {
                    var state = controller.publishClientResource(active.instanceID, path)
                    val deadline = SystemClock.elapsedRealtime() + 10_000
                    while (state.publication?.confirmed != true && SystemClock.elapsedRealtime() < deadline) {
                        kotlinx.coroutines.delay(20)
                        state = controller.stepRuntime()
                    }
                    state.publication
                }
                assertTrue("Lua Executor must confirm the Native Client resource", publication?.confirmed == true)
                val digest = MessageDigest.getInstance("SHA-256").digest(clientFile.readBytes())
                    .joinToString("") { "%02x".format(it.toInt() and 255) }
                assertEquals(digest, publication?.sha256)
            }
            ui.onNodeWithTag("plugin-runtime-state").assertExists()
            ui.onNodeWithTag("plugin-resource-${received.descriptor.resourceId}").performScrollTo().assertExists()
            if (html) {
                assertArrayEquals("body{background:#0000ff!important;min-height:200px}".toByteArray(),
                    extensions.receivedResources.single { it.descriptor.displayName == cssPath }.bytes)
                captureHTML()
                val firstView = retainedView!!; val firstClient = retainedClient!!; val firstRequest = retainedRequest!!
                ui.onNodeWithTag("plugin-resource-select-${image.descriptor.resourceId}").performScrollTo().performClick()
                ui.waitUntil(10_000) { ui.onAllNodesWithTag("plugin-resource-image").fetchSemanticsNodes().isNotEmpty() }
                assertEquals("resource selection revokes old HTML reads", 403,
                    firstClient.shouldInterceptRequest(firstView, firstRequest)!!.statusCode)
                ui.onNodeWithTag("plugin-resource-select-${received.descriptor.resourceId}").performScrollTo().performClick()
                captureHTML()
                assertNotEquals(java.net.URI(firstRequest.url.toString()).host, java.net.URI(retainedRequest!!.url.toString()).host)
            } else {
            ui.onNodeWithTag("plugin-resource-markdown").assertExists()
            ui.waitUntil(10_000) {
                ui.onAllNodesWithTag("plugin-resource-markdown-inline-image").fetchSemanticsNodes().isNotEmpty()
            }
            ui.onNodeWithTag("plugin-resource-markdown-inline-image").assertExists()
            ui.onNodeWithText("Android Markdown over $routeLabel SSH", substring = true).assertExists()
            val preview = ui.onNodeWithTag("plugin-resource-markdown").captureToImage().asAndroidBitmap()
            var redPixels = 0
            for (y in 0 until preview.height) for (x in 0 until preview.width) {
                val color = preview.getPixel(x, y)
                if (android.graphics.Color.red(color) > 240 && android.graphics.Color.green(color) < 32 &&
                    android.graphics.Color.blue(color) < 32) redPixels++
            }
            assertTrue("real SSH Markdown attachment should draw red pixels; got $redPixels", redPixels > 0)
            }

            if (originalTerminal != null) {
                val resourceIds = extensions.receivedResources.map { it.descriptor.resourceId }
                val runtime = extensions.activeRuntime
                terminalProbe.returnFromPlugins(originalTerminal, returnKeyboard!!, systemBack = !returnKeyboard)
                terminalProbe.inputProof(originalTerminal)
                terminalProbe.leaveToPlugins(returnKeyboard)
                assertNotNull(runtime)
                assertEquals("navigation must keep the same plugin Runtime instance", runtime!!.instanceID,
                    extensions.activeRuntime?.instanceID)
                assertEquals(com.hanxi.ghosterm.bridge.ExtensionRuntime.Phase.ACTIVE,
                    extensions.activeRuntime?.info?.phase)
                assertEquals("navigation must retain exact resource receipts", resourceIds,
                    extensions.receivedResources.map { it.descriptor.resourceId })
                assertArrayEquals(expectedDocument.toByteArray(), received.bytes)
                if (html) {
                    assertEquals("leaving Settings revokes its HTML view", 403,
                        retainedClient!!.shouldInterceptRequest(retainedView!!, retainedRequest!!)!!.statusCode)
                    captureHTML()
                }
            }

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
            if (html) {
                ui.waitForIdle()
                ui.onNodeWithTag("plugin-resource-html").assertDoesNotExist()
                assertEquals("stopping and releasing revokes the retained HTML view", 403,
                    retainedClient!!.shouldInterceptRequest(retainedView!!, retainedRequest!!)!!.statusCode)
            }
            assertNull("stopping and releasing the received Preview must not leave an AppModel error",
                extensions.error)
        } finally {
            terminalProbe.close()
            var closeFailure: Throwable? = null
            try { runBlocking { controller.close() } } catch (failure: Throwable) { closeFailure = failure }
            linkedRoot?.delete()
            clientFile?.delete()
            clientRoot?.delete()
            if (originalKnownHosts == null) knownHosts.delete() else knownHosts.writeBytes(originalKnownHosts)
            targetContext.contentResolver.call(providerUri, "cleanup", null, Bundle().apply {
                putString("uri", treeUri.toString()); putInt("flags", grantFlags)
            })
            closeFailure?.let { throw it }
        }
    }
}

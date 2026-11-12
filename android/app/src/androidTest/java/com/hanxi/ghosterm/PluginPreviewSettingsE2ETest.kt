package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.lifecycle.ViewModelProvider
import androidx.compose.ui.test.*
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

    @Test fun realTwoHopPreviewReceiptAppearsInMainActivitySettings() {
        assumeTrue("requires the explicit JS-enabled Android build", BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture
        val ports = payload.getJSONObject("two").getJSONArray("ports")
        val packageDir = File(instrumentation.targetContext.cacheDir, "preview-settings-${System.nanoTime()}")
        check(packageDir.mkdir())
        val packageFiles = payload.getJSONObject("previewPackageFiles")
        val names = packageFiles.keys()
        while (names.hasNext()) {
            val name = names.next()
            val file = File(packageDir, name)
            check(file.canonicalPath.startsWith(packageDir.canonicalPath + File.separator))
            check(file.parentFile!!.mkdirs() || file.parentFile!!.isDirectory)
            file.writeBytes(Base64.decode(packageFiles.getString(name), Base64.DEFAULT))
        }
        val digest = MessageDigest.getInstance("SHA-256").digest(File(packageDir, "integrity.json").readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals(payload.getString("previewPackageDigest"), digest)

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
            ui.onNodeWithText("选择插件文件夹").assertExists()

            val review = runBlocking { controller.review(packageDir) }
            assertEquals("org.ghosterm.preview", review.id)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK,
                runBlocking { controller.approve(review.id, review.digest) })
            val command = review.commands.single { it.path == "preview" }
            val entry = ExtensionPackagesViewModel.Entry(review, "已批准（本进程）")
            val request = ExtensionRelayBootstrap.Request(
                executable = payload.getString("previewGtermWrapper"), executorDirectory = payload.getString("previewPackageDirectory"),
                executorDigest = digest, clientDigest = digest, methods = emptyList(), command = "preview",
                arguments = listOf("fixture.txt"), workspaceRootDirectory = payload.getString("previewWorkspace"))

            extensions.launch(entry, command, target, request, sessions)
            val authDeadline = SystemClock.elapsedRealtime() + 90_000
            var handledPrompt: Triple<Long, Int, SSHAuthPhase>? = null
            while (main { extensions.activeSession == null } && SystemClock.elapsedRealtime() < authDeadline) {
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
            await("Settings page did not reach Session consent: session=${main { extensions.activeSession?.info }} " +
                "error=${main { extensions.error }}") {
                extensions.activeSession?.info?.phase == com.hanxi.ghosterm.bridge.ExtensionSession.Phase.CONSENT
            }
            ui.onNodeWithTag("plugin-consent-remote-command")
                .assertTextEquals(extensions.activeSession!!.remoteCommand)
            ui.onNodeWithText("需要能力：resource.receive, file.readScoped").assertExists()
            ui.onNodeWithText("允许并启动").performClick()

            await("MainActivity Settings did not receive Preview resource") {
                extensions.receivedResources.any { it.descriptor.displayName == "fixture.txt" }
            }
            val received = extensions.receivedResources.single { it.descriptor.displayName == "fixture.txt" }
            assertArrayEquals("Android Preview over two-hop SSH\n".toByteArray(), received.bytes)
            assertEquals("text/plain", received.descriptor.mime)
            ui.onNodeWithTag("plugin-runtime-state").assertExists()
            ui.onNodeWithTag("plugin-resource-${received.descriptor.resourceId}").performScrollTo().assertExists()
            ui.onNodeWithText("Android Preview over two-hop SSH", substring = true).assertExists()
        } finally {
            var closeFailure: Throwable? = null
            try { runBlocking { controller.close() } } catch (failure: Throwable) { closeFailure = failure }
            if (originalKnownHosts == null) knownHosts.delete() else knownHosts.writeBytes(originalKnownHosts)
            packageDir.deleteRecursively()
            closeFailure?.let { throw it }
        }
    }
}

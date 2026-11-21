package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.DataInputStream
import java.io.File
import java.net.Socket
import java.security.MessageDigest

/** Actual production ViewModel, consent/form UI and shared official Executor. */
@RunWith(AndroidJUnit4::class)
class PluginFormSSHAppTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun fixture(): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", checkNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((checkNotNull(args.getString("fixtureToken")) + "\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val size = input.readInt()
            require(size in 1..1048576)
            return JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
    }
    private fun <T> main(block: () -> T): T {
        var result: T? = null; instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String, check: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 20000
        while (!check() && SystemClock.elapsedRealtime() < end) SystemClock.sleep(20)
        assertTrue(label, check())
    }
    @Test fun officialFormThroughViewModelConsentAndNativeControls() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture(); val jump = payload.getString("route") == "jump"
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "form-${java.util.UUID.randomUUID()}"); check(root.mkdir())
        val files = payload.getJSONObject("packageFiles")
        for (name in files.keys()) {
            require(!name.startsWith('/') && name.split('/').none { it.isEmpty() || it == ".." })
            File(root, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val hash = MessageDigest.getInstance("SHA-256").digest(File(root, "integrity.json").readBytes()).joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals(payload.getString("packageDigest"), hash)
        val known = File(context.filesDir, "home/.ssh/known_hosts"); known.parentFile!!.mkdirs()
        val original = known.takeIf(File::exists)?.readBytes()
        val ports = payload.getJSONArray("ports"); val hostKeys = payload.getJSONArray("hostKeys")
        known.writeText((0..1).joinToString("") { "[127.0.0.1]:${ports.getInt(it)} ${hostKeys.getString(it)}" })
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        val owner = checkNotNull(extensions.controller)
        try {
            val review = runBlocking { owner.review(root) }
            assertTrue("ui.form" in review.requiredCapabilities)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, hash) })
            val target = PreparedSSH("form-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("form-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), hash, hash,
                listOf("form.status", "form.cancel"), command = "form", arguments = emptyList())
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            fun startForm() {
                runBlocking {
                    withContext(Dispatchers.Main) {
                        val entry = ExtensionPackagesViewModel.Entry(review, "已批准（本进程）")
                        val command = review.commands.single()
                        extensions.submitLaunch(entry, command, target, request, sessions, null)
                    }
                }
                var handled: Triple<Long, Int, SSHAuthPhase>? = null
                val deadline = SystemClock.elapsedRealtime() + 60000
                while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < deadline) {
                    val prompt = main { sessions.connecting?.auth }
                    val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                    if (prompt == null || id == handled) { SystemClock.sleep(20); continue }; handled = id
                    when (prompt.phase) {
                        SSHAuthPhase.CREDENTIALS -> runBlocking { owner.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                        SSHAuthPhase.HOSTKEY -> fail("Only explicitly pinned owned hosts may be used")
                        SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned form SSH authentication failed")
                        else -> SystemClock.sleep(20)
                    }
                }
                assertNull(main { extensions.error })
                await("Form Session consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
                assertEquals(setOf("session.rpc", "ui.form"), main { extensions.activeSession?.requiredCapabilities?.toSet() })
                assertEquals(listOf(PluginForm.method), main { extensions.activeSession?.hostMethods })
                ui.onNodeWithText("允许并启动").performClick()
                await("Form Client activation: ${main { extensions.error }}") { main { extensions.pendingPluginForm != null || extensions.error != null } }
                assertNull(main { extensions.error })
            }
            fun status(method: String = "form.status") = runBlocking {
                withContext(Dispatchers.Main) {
                    JSONObject(extensions.requestPluginViewOutcome(method, "{}".toByteArray()).decodeToString())
                        .getJSONObject("value")
                }
            }
            fun stopForm() {
                main { extensions.stopRuntime() }
                await("Form Runtime stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.ACTIVE,
                    ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.CREATED) } }
                ui.onNodeWithText("释放插件资源").performScrollTo().assertIsEnabled().performClick()
                await("Form owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }
                assertNull(main { extensions.error })
            }

            startForm()
            val first = main { checkNotNull(extensions.pendingPluginForm) }
            ui.onNodeWithTag("plugin-form-field-name").performScrollTo().performTextInput("Sample")
            ui.onNodeWithTag("plugin-form-field-layout").performSemanticsAction(SemanticsActions.OnClick)
            ui.onNodeWithText("Compact").performSemanticsAction(SemanticsActions.OnClick)
            ui.onNodeWithTag("plugin-form-submit").assertIsEnabled().performSemanticsAction(SemanticsActions.OnClick)
            await("Form submitted") { main { extensions.pendingPluginForm == null && !extensions.answeringPluginForm } }
            assertEquals("submitted", status().getString("status"))
            stopForm()

            startForm()
            val second = main { checkNotNull(extensions.pendingPluginForm) }
            assertEquals(first.form.id, second.form.id)
            assertNotEquals(first.instanceID, second.instanceID)
            main { extensions.answerPluginForm(first.token, PluginForm.Answer.Dismissed) }
            assertEquals("pending", status().getString("status"))
            assertSame(second.token, main { extensions.pendingPluginForm?.token })
            assertTrue(status("form.cancel").getBoolean("requested"))
            await("Executor cancellation removes the form") { main { extensions.pendingPluginForm == null } }
            main { extensions.answerPluginForm(second.token, PluginForm.Answer.Dismissed) }
            assertEquals("cancelled", status().getString("status"))
            stopForm()
        } finally {
            main { if (!extensions.launchBusy) extensions.stopRuntime() }
            runBlocking { owner.requestStopRuntime(); owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

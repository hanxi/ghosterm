package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.DataInputStream
import java.io.File
import java.net.Socket
import java.security.MessageDigest

/** Actual production ViewModel, consent/paging UI and shared official Executor. */
@RunWith(AndroidJUnit4::class)
class PluginTaskSSHAppTest {
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
    @Test fun officialTaskThroughViewModelConsentAndNativeCancellation() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture(); val limit = payload.getInt("limit"); val scenario = payload.getString("scenario"); val jump = payload.getString("route") == "jump"
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "task-${java.util.UUID.randomUUID()}"); check(root.mkdir())
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
        var capturedTask: PluginTaskModel? = null
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(listOf("task"), review.commands.single().views)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, hash) })
            val target = PreparedSSH("task-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("task-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), hash, hash,
                emptyList(), command = "task primes", arguments = listOf("--limit", limit.toString()))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            main { extensions.launch(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(), target, request, sessions) }
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            val deadline = SystemClock.elapsedRealtime() + 60000
            while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < deadline) {
                val prompt = main { sessions.connecting?.auth }
                val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || id == handled) { SystemClock.sleep(20); continue }; handled = id
                when (prompt.phase) {
                    SSHAuthPhase.CREDENTIALS -> runBlocking { owner.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.HOSTKEY -> fail("Only explicitly pinned owned hosts may be used")
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned task SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { extensions.error })
            await("Task Session consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(listOf("session.rpc", "ui.task"), main { extensions.activeSession?.requiredCapabilities })
            assertEquals(PluginTaskModel.methods, main { extensions.activeSession?.methods })
            if (main { extensions.activeSession?.methods } != PluginTaskModel.methods) return
            ui.onNodeWithText("允许并启动").performClick()
            await("Task Client activation") { main { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
            val task = main { extensions.task }; assertNotNull("Native task contribution must be wired", task)
            if (task == null) return
            capturedTask = task
            // Drive the test frame clock through the production contribution;
            // polling the worker alone does not compose its LaunchedEffect.
            ui.onNodeWithTag("plugin-task").assertExists()
            await("First Native page: ${main { extensions.error }}") { main { task.state.value.snapshot != null || task.state.value.issue != null } }
            assertNull(main { task.state.value.issue }); assertEquals((limit - 1).coerceAtLeast(0).toString(), main { task.state.value.snapshot?.total })
            var declaredFailure = false
            try {
                val outcome = runBlocking { kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main.immediate) {
                    extensions.requestPluginViewOutcome("view.task.read", "{}".toByteArray())
                } }
                PluginTaskCodec.descriptor(outcome)
            } catch (failure: PluginTaskCodec.RPCFailure) { declaredFailure = failure.code == "INVALID_INPUT" && failure.effect == "none" } catch (_: Exception) { }
            assertTrue("Production Native RPC must preserve declared input failure", declaredFailure)
            if (!declaredFailure) return
            if (scenario == "cancel") {
                assertTrue(main { task.canCancel && task.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.RUNNING })
                ui.onNodeWithTag("plugin-task-cancel").performScrollTo().performClick()
                await("Native cancellation confirmation") {
                    ui.mainClock.advanceTimeBy(600)
                    main { task.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.CANCELLED }
                }
                assertEquals(PluginTaskSnapshot.Result.Cancelled("none"), main { task.state.value.snapshot?.result })
                assertNotNull(main { task.state.value.cancelRequestID })
                assertTrue(main { task.state.value.snapshot!!.completed.toULong() < (limit - 1).toULong() })
            } else {
                await("Native task completion") {
                    ui.mainClock.advanceTimeBy(600)
                    main { task.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.SUCCEEDED }
                }
                assertEquals(if (limit == 10000) "1229 primes up to 10000" else "0 primes up to 0", main { (task.state.value.snapshot?.result as? PluginTaskSnapshot.Result.Success)?.summary })
                assertNull(main { task.state.value.cancelRequestID })
            }
            assertFalse(main { task.canCancel }); assertNull(main { task.state.value.issue })
            val last = main { task.state.value.snapshot }
            main { extensions.stopRuntime() }
            await("Task Runtime stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.ACTIVE,
                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.CREATED) } }
            assertTrue(main { !task.state.value.valid && task.state.value.snapshot == last })
            ui.onNodeWithText("释放插件资源").performScrollTo().assertIsEnabled().performClick()
            await("Task owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }
            assertNull(main { extensions.error })
        } finally {
            main { capturedTask?.invalidate(); if (!extensions.launchBusy) extensions.stopRuntime() }
            runBlocking { owner.requestStopRuntime(); owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

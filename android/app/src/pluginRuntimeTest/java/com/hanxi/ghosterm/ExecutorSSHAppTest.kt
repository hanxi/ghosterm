package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.PreparedSSH
import com.hanxi.ghosterm.bridge.SSHAuthPhase
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

/** Real production ViewModel/consent UI/Client -> SSH -> gterm -> Node/native Exec. */
@RunWith(AndroidJUnit4::class)
class ExecutorSSHAppTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun service(action: String = "fixture"): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", checkNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((checkNotNull(args.getString("fixtureToken")) + "\n$action\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val size = input.readInt()
            require(size in 1..1_048_576)
            return JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
    }
    private fun <T> main(block: () -> T): T {
        var value: T? = null
        instrumentation.runOnMainSync { value = block() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(label: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 20_000
        while (SystemClock.elapsedRealtime() < end) { if (predicate()) return; SystemClock.sleep(20) }
        fail(label)
    }
    @Test fun remoteExecutionThroughViewModelAndConsentUI() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = service(); val mode = payload.getString("mode"); val jump = payload.getString("route") == "jump"
        val context = instrumentation.targetContext
        val folder = File(context.cacheDir, "execution-${java.util.UUID.randomUUID()}"); check(folder.mkdir())
        val files = payload.getJSONObject("packageFiles")
        for (name in files.keys()) {
            check(!name.startsWith('/') && name.split('/').none { it == ".." || it.isEmpty() })
            File(folder, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val digest = MessageDigest.getInstance("SHA-256").digest(File(folder, "integrity.json").readBytes()).joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals(payload.getString("packageDigest"), digest)
        val known = File(context.filesDir, "home/.ssh/known_hosts"); known.parentFile!!.mkdirs()
        val original = known.takeIf(File::exists)?.readBytes()
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        val controller = checkNotNull(extensions.controller)
        try {
            val review = runBlocking { controller.review(folder) }
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { controller.approve(review.id, digest) })
            val ports = payload.getJSONArray("ports"); val user = payload.getString("user")
            val target = PreparedSSH("execution-target", "127.0.0.1", user, ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("execution-jump", "127.0.0.1", user, ports.getInt(0), emptyList())) else emptyList())
            val selected = if (payload.getString("runtime") == "exec") ExtensionRelayBootstrap.Execution.Runtime.Exec
                else ExtensionRelayBootstrap.Execution.Runtime.Node(ExtensionRelayBootstrap.NodeDependency(payload.getString("node"),
                    payload.getString("nodeVersion"), payload.getString("nodeDigest"), payload.getString("sdk"), payload.getString("sdkDigest")))
            val execution = ExtensionRelayBootstrap.Execution(selected, payload.getString("temporary"), mode != "no-approval")
            val request = ExtensionRelayBootstrap.Request(payload.getString("wrapper"), payload.getString("packageDirectory"), digest, digest,
                listOf("fixture.echo"), command = "fixture", arguments = listOf("中文😀", "", "literal'$(owned)"),
                processEnvironmentNames = listOf("GT_EXECUTION_APP_FIXTURE"), execution = execution)
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            main { extensions.launch(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(), target, request, sessions) }
            if (mode == "no-approval") {
                assertNotNull(main { extensions.error }); assertNull(main { extensions.activeSession })
                assertNull(main { sessions.connecting }); assertFalse(service("audit").getBoolean("wrapperStarted")); return
            }
            val end = SystemClock.elapsedRealtime() + 60_000
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            var hostCount = 0
            while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < end) {
                val attempt = main { sessions.connecting }; val prompt = attempt?.auth
                val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || id == null || id == handled) { SystemClock.sleep(20); continue }
                handled = id
                when (prompt.phase) {
                    SSHAuthPhase.HOSTKEY -> { hostCount++; main { sessions.decide(checkNotNull(attempt).id, prompt, true) } }
                    SSHAuthPhase.CREDENTIALS -> runBlocking { controller.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("owned SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { extensions.error })
            await("Session did not reach consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(if (jump) 2 else 1, hostCount)
            assertEquals(execution, main { extensions.activeSession?.executorExecution })
            assertEquals(listOf("session.rpc"), main { extensions.activeSession?.requiredCapabilities })
            assertFalse("No module execution before consent", service("audit").getBoolean("activated"))
            assertFalse("No Executor entry before consent", service("audit").getBoolean("entered"))
            ui.onNodeWithTag("plugin-consent-executor-runtime").assertExists()
            if (mode == "deny") {
                ui.onNodeWithText("拒绝", useUnmergedTree = true).performClick()
                await("denial did not end Session") { main { !extensions.launchBusy && extensions.activeSession?.info?.phase == ExtensionSession.Phase.ENDED } }
                assertFalse(service("audit").getBoolean("activated"))
                main { extensions.releaseFinished() }; await("denied owners not released") { main { extensions.activeSession == null && !extensions.launchBusy } }
                assertNull(main { extensions.error }); return
            }
            ui.onNodeWithText("允许并启动").performClick()
            await("Client/Executor did not activate") { main { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } && service("audit").getBoolean("activated") }
            assertEquals(payload.getString("temporary"), service("audit").getString("environment"))
            val input = JSONObject().put("text", "echo 中文😀 '\" }").put("count", 17)
            val admission = runBlocking { controller.requestRuntimeExecutor("fixture.echo", input.toString().toByteArray()) }
            assertEquals(ExtensionSession.RPCStatus.OK, admission.status); val handle = checkNotNull(admission.handle)
            var result: ExtensionSession.RPCResult? = null
            await("RPC echo did not confirm") {
                val (status, value) = runBlocking { controller.runtimeExecutorRequestResult(handle) }
                assertEquals(ExtensionSession.RPCStatus.OK, status); result = value; value?.confirmed == true
            }
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, result!!.state)
            val outcome = JSONObject(result!!.outcome.toString(Charsets.UTF_8)); assertEquals("success", outcome.getString("kind"))
            assertEquals(input.getString("text"), outcome.getJSONObject("value").getString("text"))
            assertEquals(17, outcome.getJSONObject("value").getInt("count"))
            assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { controller.forgetRuntimeExecutorRequest(handle) })
            if (mode == "pending-cancel" || mode == "pending-loss") {
                val pending = runBlocking { controller.requestRuntimeExecutor("fixture.echo", "{\"text\":\"WAIT\",\"count\":99}".toByteArray()) }
                val pendingHandle = checkNotNull(pending.handle)
                await("Remote handler did not start") { service("audit").getBoolean("handler") }
                if (mode == "pending-loss") service("kill")
                else {
                    assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { controller.cancelRuntimeExecutorRequest(pendingHandle) })
                    val (_, before) = runBlocking { controller.runtimeExecutorRequestResult(pendingHandle) }
                    assertTrue(checkNotNull(before).cancelRequested); assertFalse(before.confirmed)
                    controller.requestStopRuntime()
                }
                await("Pending interruption did not become unknown") {
                    val (status, value) = runBlocking { controller.runtimeExecutorRequestResult(pendingHandle) }
                    assertEquals(ExtensionSession.RPCStatus.OK, status); result = value
                    value?.state == ExtensionSession.RPCState.UNKNOWN
                }
                assertFalse("Interruption is not a remote business receipt", result!!.confirmed)
            }
            main { extensions.stopRuntime() }
            await("Client Runtime did not stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE) } }
            main { extensions.releaseFinished() }
            await("Client owners not released") { main { !extensions.launchBusy && extensions.activeSession == null && extensions.activeRuntime == null } }
            assertNull(main { extensions.error })
        } finally {
            runBlocking { controller.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            folder.deleteRecursively()
        }
    }
}

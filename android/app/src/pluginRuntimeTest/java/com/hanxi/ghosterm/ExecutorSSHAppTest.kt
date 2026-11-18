package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.ExtensionFileSource
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
    private fun openOrdinaryTerminal(sessions: SessionsViewModel, target: PreparedSSH, pem: String): TerminalSession {
        val existing = main { sessions.sessions.map { it.id }.toSet() }
        main { sessions.connect(target) }
        var handled: Triple<Long, Int, SSHAuthPhase>? = null
        var terminal: TerminalSession? = null
        await("Ordinary SSH terminal did not connect") {
            main {
                val attempt = sessions.connecting
                val prompt = attempt?.auth
                val identity = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt != null && identity != handled) {
                    handled = identity
                    when (prompt.phase) {
                        SSHAuthPhase.HOSTKEY -> sessions.decide(attempt.id, prompt, true)
                        SSHAuthPhase.CREDENTIALS -> checkNotNull(attempt.connection).privateKey(prompt, pem.toByteArray())
                        SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Ordinary SSH authentication failed")
                        else -> Unit
                    }
                }
                terminal = sessions.sessions.firstOrNull { it.id !in existing && it.state.connected }
                terminal != null
            }
        }
        return checkNotNull(terminal)
    }

    private fun verifyOrdinaryTerminal(terminal: TerminalSession, stage: String) {
        main {
            terminal.surface.commit("printf '%s_%s_' GHOSTERM $stage; " +
                "printf '\\000\\377\\033\\177\\303\\251' | /usr/bin/od -An -tx1 | /usr/bin/tr -d ' '\r")
        }
        await("Ordinary SSH terminal grid missing the remote byte receipt ($stage)") {
            main { terminal.surface.frame(-1)?.accessibilityText()?.contains("GHOSTERM_${stage}_00ff1b7fc3a9") == true }
        }
    }

    @Test fun remoteExecutionThroughViewModelAndConsentUI() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = service(); val mode = payload.getString("mode"); val jump = payload.getString("route") == "jump"
        val clientCase = mode.startsWith("client-")
        val resourceCase = mode.startsWith("resource") || clientCase
        val processCase = mode.startsWith("process")
        val processPending = mode == "process-stop" || mode == "process-loss"
        val context = instrumentation.targetContext
        val folder = File(context.cacheDir, "execution-${java.util.UUID.randomUUID()}"); check(folder.mkdir())
        val local = File(context.cacheDir, "execution-client-${java.util.UUID.randomUUID()}")
        var clientWorkspace: ExtensionController.ClientWorkspace? = null
        if (clientCase) {
            check(local.mkdir())
            File(local, "upload.bin").writeBytes(ByteArray(262144) { ((it * 7 + 3) % 251).toByte() })
            val outside = File(folder, "../${local.name}-outside.bin").canonicalFile
            outside.writeText("UNAUTHORIZED owned fixture")
            android.system.Os.symlink(outside.path, File(local, "link.bin").path)
            val selectedPath = if (mode == "client-directory-link") {
                File(local.parentFile, "${local.name}-directory-link").also { android.system.Os.symlink(local.path, it.path) }.path
            } else if (mode == "client-directory-file") File(local, "upload.bin").path else local.path
            clientWorkspace = ExtensionController.ClientWorkspace(selectedPath, mode != "client-no-approval")
        }
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
        var ordinaryTerminal: TerminalSession? = null
        try {
            val review = runBlocking { controller.review(folder) }
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { controller.approve(review.id, digest) })
            val ports = payload.getJSONArray("ports"); val user = payload.getString("user")
            val target = PreparedSSH("execution-target", "127.0.0.1", user, ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("execution-jump", "127.0.0.1", user, ports.getInt(0), emptyList())) else emptyList())
            val selected = if (payload.getString("runtime") == "exec") ExtensionRelayBootstrap.Execution.Runtime.Exec
                else ExtensionRelayBootstrap.Execution.Runtime.Node(ExtensionRelayBootstrap.NodeDependency(payload.getString("node"),
                    payload.getString("nodeVersion"), payload.getString("nodeDigest"), payload.getString("sdk"), payload.getString("sdkDigest")))
            val attaching = payload.has("ipcDirectory")
            val execution = if (attaching) null else ExtensionRelayBootstrap.Execution(selected, payload.getString("temporary"), mode != "no-approval")
            val attachment = if (attaching) ExtensionRelayBootstrap.Attachment(payload.getString("ipcDirectory"), payload.getString("ipcInstance"), mode != "no-approval", processCase) else null
            val request = ExtensionRelayBootstrap.Request(payload.getString("wrapper"), if (attaching) "" else payload.getString("packageDirectory"), digest, digest,
                listOf("fixture.echo"), command = "fixture", arguments = listOf("中文😀", "", "literal'$(owned)"),
                workspaceRootDirectory = if (!attaching && (resourceCase || processCase)) payload.getString("workspace") else null,
                processPrograms = if (!attaching && processCase) listOf(ExtensionRelayBootstrap.ProcessProgram("cat", payload.getString("processExecutable"), "fixture", payload.getString("processDigest"))) else emptyList(),
                processEnvironmentNames = if (attaching) emptyList() else listOf("GT_EXECUTION_APP_FIXTURE") + if (mode == "process-environment") listOf("GT_SELECTED") else emptyList(), execution = execution, attachment = attachment)
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            main { extensions.launch(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(), target, request, sessions, clientWorkspace) }
            if (mode in setOf("no-approval", "client-no-approval", "client-undeclared")) {
                assertNotNull(main { extensions.error }); assertNull(main { extensions.activeSession })
                assertNull(main { sessions.connecting }); assertEquals(attaching,service("audit").getBoolean("wrapperStarted"))
                if (attaching) { assertFalse(service("audit").getBoolean("attachStarted")); assertTrue(service("audit").getBoolean("ipcListening"));service("stop-front") }
                return
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
            if (mode.startsWith("attach-")) {
                await("mismatched foreground did not end Native Session") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.ENDED || extensions.error != null } }
                assertNull(main { extensions.activeRuntime });assertFalse(service("audit").getBoolean("activated"));assertFalse(service("audit").getBoolean("entered"))
                assertTrue(service("audit").getBoolean("attachStarted"));assertTrue(service("audit").getBoolean("ipcListening"))
                service("stop-front");main { extensions.releaseFinished() }
                await("rejected attach owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }
                return
            }
            assertNull(main { extensions.error })
            await("Session did not reach consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(if (jump) 2 else 1, hostCount)
            assertEquals(execution, main { extensions.activeSession?.executorExecution })
            assertEquals(attachment, main { extensions.activeSession?.executorAttachment })
            assertEquals(clientWorkspace, main { extensions.activeSession?.clientWorkspace })
            assertFalse(main { checkNotNull(extensions.activeSession).remoteCommand.contains(local.path) })
            val required = listOf("session.rpc") + (if (resourceCase) listOf("resource.receive", "file.readScoped") else emptyList()) +
                (if (processCase) listOf("process.spawn", "process.pipes") else emptyList())
            assertEquals(required.toSet(),
                main { extensions.activeSession?.requiredCapabilities?.toSet() })
            assertFalse("No module execution before consent", service("audit").getBoolean("activated"))
            assertFalse("No Executor entry before consent", service("audit").getBoolean("entered"))
            ui.onNodeWithTag("plugin-consent-executor-runtime").assertExists()
            ui.onNodeWithTag("plugin-consent-client-workspace").assertExists()
            if (clientCase) ui.onNodeWithTag("plugin-consent-client-workspace").assertTextEquals(clientWorkspace!!.reviewDescription)
            if (mode == "deny") {
                ui.onNodeWithText("拒绝", useUnmergedTree = true).performClick()
                await("denial did not end Session") { main { !extensions.launchBusy && extensions.activeSession?.info?.phase == ExtensionSession.Phase.ENDED } }
                assertFalse(service("audit").getBoolean("activated"))
                main { extensions.releaseFinished() }; await("denied owners not released") { main { extensions.activeSession == null && !extensions.launchBusy } }
                assertNull(main { extensions.error }); return
            }
            ui.onNodeWithText("允许并启动").performClick()
            if (mode == "client-directory-link" || mode == "client-directory-file") {
                await("Invalid Native directory must fail activation") { main { !extensions.launchBusy } }
                assertNotNull(main { extensions.error }); assertNull(main { extensions.activeRuntime })
                return
            }
            await("Client/Executor did not activate") {
                val state = main { extensions.error to extensions.activeRuntime?.info }
                if (state.first != null || state.second?.phase in setOf(ExtensionRuntime.Phase.FAILED, ExtensionRuntime.Phase.STOPPED, ExtensionRuntime.Phase.CLOSED)) {
                    fail("Native activation failed: error=${state.first}; runtime=${state.second}; remote=${service("audit")}")
                }
                state.second?.phase == ExtensionRuntime.Phase.ACTIVE && service("audit").getBoolean("activated")
            }
            assertEquals(payload.getString("temporary"), service("audit").getString("environment"))
            val foregroundTmux = payload.optBoolean("foregroundTmux")
            assertEquals(if (foregroundTmux) "[true,true,true]" else "[false,false,false]", service("audit").getJSONArray("tty").toString())
            assertEquals("[false,false,false]", service("audit").getJSONArray("runtimeTTY").toString())
            if (mode == "allow" && !attaching) {
                ordinaryTerminal = openOrdinaryTerminal(sessions, target, payload.getString("pem"))
                verifyOrdinaryTerminal(ordinaryTerminal, "ACTIVE")
                assertEquals(ExtensionRuntime.Phase.ACTIVE, main { extensions.activeRuntime?.info?.phase })
            }
            if (!payload.isNull("tmuxTopology")) {
                fun probe(label: String) {
                    val admission = runBlocking { controller.requestRuntimeExecutor("fixture.echo", JSONObject().put("text", label).put("count", 23).toString().toByteArray()) }
                    val handle = checkNotNull(admission.handle)
                    await("Plugin did not confirm after terminal routing changed") { runBlocking { controller.runtimeExecutorRequestResult(handle).second?.confirmed == true } }
                    val result = checkNotNull(runBlocking { controller.runtimeExecutorRequestResult(handle).second })
                    assertEquals(ExtensionSession.RPCState.SUCCEEDED, result.state)
                    val value = JSONObject(result.outcome.toString(Charsets.UTF_8)).getJSONObject("value")
                    assertEquals(label, value.getString("text")); assertEquals(23, value.getInt("count"))
                    assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { controller.forgetRuntimeExecutorRequest(handle) })
                }
                service("tmux-pane"); probe("TMUX_PANE")
                service("tmux-window"); probe("TMUX_WINDOW")
                if (foregroundTmux) { service("tmux-front"); probe("TMUX_FRONT") }
                if (payload.optBoolean("stopTerminal")) { service("tmux-stop-terminal"); probe("TMUX_CLOSED") }
            }
            if (clientCase) {
                var rejected = false
                try { runBlocking { controller.publishClientResource("obsolete-runtime", "upload.bin") } }
                catch (_: IllegalStateException) { rejected = true }
                assertTrue("Stale Native action must not publish into another Runtime", rejected)
                assertNull(main { extensions.activeRuntime?.publication })
            }
            if (clientCase) {
                val path = if (mode == "client-parent") "../${local.name}-outside.bin" else if (mode == "client-link") "link.bin" else "upload.bin"
                if (mode == "client-resource") {
                    ui.onNodeWithTag("plugin-client-publication-path").performScrollTo().performTextInput(path)
                    androidx.test.espresso.Espresso.closeSoftKeyboard()
                    ui.onNodeWithTag("plugin-client-publish").performScrollTo().performClick()
                } else main { extensions.publishClientResource(path, if (mode == "client-root") "outside" else "client") }
                if (mode in setOf("client-stop", "client-loss", "client-cancel")) {
                    await("Native publication did not enter a partial send") {
                        main { extensions.activeRuntime?.publication?.let { it.phase == ExtensionFileSource.Phase.SENDING && it.accepted > 0 && it.accepted < 262144 } == true }
                    }
                    assertFalse(main { checkNotNull(extensions.activeRuntime?.publication).confirmed })
                    if (mode == "client-cancel") {
                        ui.onNodeWithTag("plugin-client-publication-cancel").performScrollTo().performClick()
                        await("Cancelled Source did not release") { main { extensions.activeRuntime?.publication?.released == true } }
                        val publication = main { checkNotNull(extensions.activeRuntime?.publication) }
                        assertEquals(ExtensionFileSource.Phase.STOPPED, publication.phase)
                        assertEquals(ExtensionFileSource.Status.CANCELLED, publication.error)
                        assertFalse(publication.expired)
                    } else {
                        if (mode == "client-loss") service("kill") else controller.requestStopRuntime()
                        await("Interrupted Runtime did not stop") { main { extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE) } }
                        val publication = main { checkNotNull(extensions.activeRuntime?.publication) }
                        assertFalse(publication.confirmed)
                        assertNotEquals(ExtensionFileSource.Phase.SUCCEEDED, publication.phase)
                    }
                    main { extensions.stopRuntime() }
                    await("Client Runtime did not stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE) } }
                    main { extensions.releaseFinished() }
                    await("Interrupted owners not released") { main { !extensions.launchBusy && extensions.activeRuntime == null && extensions.activeSession == null } }
                    assertNull(main { extensions.error }); return
                }
                await("Native publication did not release its Source") { main { extensions.activeRuntime?.publication?.released == true } }
                val publication = main { checkNotNull(extensions.activeRuntime?.publication) }
                assertFalse(publication.expired); assertEquals(ExtensionFileSource.Status.OK, publication.cleanupError)
                if (mode == "client-resource") {
                    assertEquals(ExtensionFileSource.Phase.SUCCEEDED, publication.phase)
                    assertTrue(publication.confirmed); assertEquals(262144L, publication.bytes)
                    val bytes = ByteArray(262144) { ((it * 7 + 3) % 251).toByte() }
                    assertEquals(MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }, publication.sha256)
                } else {
                    assertEquals(ExtensionFileSource.Phase.FAILED, publication.phase); assertFalse(publication.confirmed)
                    assertEquals(if (mode == "client-link") ExtensionFileSource.Status.UNSUPPORTED else ExtensionFileSource.Status.DENIED, publication.error)
                }
            }
            val specialized = resourceCase || (processCase && !processPending)
            val input = JSONObject().put("text", if (specialized) mode.replace('-', '_').uppercase(java.util.Locale.ROOT) else "echo 中文😀 '\" }").put("count", 17)
            val admission = runBlocking { controller.requestRuntimeExecutor("fixture.echo", input.toString().toByteArray()) }
            assertEquals(ExtensionSession.RPCStatus.OK, admission.status); val handle = checkNotNull(admission.handle)
            var result: ExtensionSession.RPCResult? = null
            await("RPC echo did not confirm") {
                val (status, value) = runBlocking { controller.runtimeExecutorRequestResult(handle) }
                assertEquals(ExtensionSession.RPCStatus.OK, status); result = value; value?.confirmed == true
            }
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, result!!.state)
            val outcome = JSONObject(result!!.outcome.toString(Charsets.UTF_8)); assertEquals("success", outcome.getString("kind"))
            val value = outcome.getJSONObject("value")
            if (processCase && !processPending) {
                val proof = service("audit").getJSONObject("processProof")
                if (mode == "process-root" || mode == "process-program") {
                    assertEquals("DENIED", value.getString("text")); assertEquals(0, value.getInt("count"))
                    assertTrue(proof.getBoolean("rejected"))
                } else if (mode == "process-cancel") {
                    assertEquals("cancelled", value.getString("text")); assertEquals(payload.getInt("processSignal"), value.getInt("count"))
                    assertEquals(payload.getInt("processSignal"), proof.getInt("signal")); assertTrue(proof.getBoolean("reaped"))
                } else {
                    val count = value.getInt("count"); val digest = value.getString("text")
                    assertTrue(proof.getBoolean("reaped")); assertEquals(0, proof.getInt("signal"))
                    assertEquals(digest, proof.getString("sha256")); assertEquals(count, proof.getInt("bytes"))
                    if (mode == "process-stderr") { assertTrue(count in 1..8192); assertEquals(64, digest.length); assertNotEquals(0, proof.getInt("code")) }
                    else { assertEquals(0, proof.getInt("code")); assertEquals(payload.getInt("processOutputSize"), count); assertEquals(payload.getString("processOutputDigest"), digest) }
                }
            } else { assertEquals(input.getString("text"), value.getString("text")); assertEquals(17, value.getInt("count")) }
            assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { controller.forgetRuntimeExecutorRequest(handle) })
            if (mode == "resource" || clientCase) {
                await("Resource did not reach ViewModel") { main { extensions.receivedResources.size == 1 } }
                val resource = main { extensions.receivedResources.single() }
                val expected = ByteArray(262144) { (it % 251).toByte() }
                assertArrayEquals(expected, resource.bytes)
                val digest = MessageDigest.getInstance("SHA-256").digest(expected).joinToString("") { "%02x".format(it.toInt() and 255) }
                assertEquals(payload.getString("resourceDigest"), digest)
                assertEquals(digest, resource.descriptor.sha256)
                assertEquals("file.bin", resource.descriptor.displayName)
                assertEquals("application/octet-stream", resource.descriptor.mime)
                assertEquals(262144L, resource.descriptor.size)
                assertEquals("$digest 262144", service("audit").getString("resourceReceipt"))
            } else if (resourceCase) {
                assertTrue(main { extensions.receivedResources.isEmpty() })
                assertEquals(if (mode == "resource-link") "UNSUPPORTED" else "DENIED", service("audit").getString("resourceError"))
                assertFalse(service("audit").has("resourceReceipt"))
            }
            if (clientCase) {
                val audit = service("audit")
                if (mode == "client-resource") assertEquals(main { checkNotNull(extensions.activeRuntime?.publication).sha256 } + " 262144", audit.getString("clientReceived"))
                else assertFalse(audit.has("clientReceived"))
            }
            if (mode == "pending-cancel" || mode == "pending-loss" || processPending) {
                val text = if (processPending) mode.replace('-', '_').uppercase(java.util.Locale.ROOT) else "WAIT"
                val pending = runBlocking { controller.requestRuntimeExecutor("fixture.echo", JSONObject().put("text", text).put("count", 99).toString().toByteArray()) }
                val pendingHandle = checkNotNull(pending.handle)
                await("Remote handler did not start") { service("audit").getBoolean("handler") }
                if (processPending) assertTrue("Business child must be observed live before interruption", service("audit").getJSONArray("businessLivePids").length() > 0)
                if (mode == "pending-loss" || mode == "process-loss") service("kill")
                else if (mode == "process-stop") controller.requestStopRuntime()
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
            ordinaryTerminal?.let { verifyOrdinaryTerminal(it, "STOPPED") }
        } finally {
            ordinaryTerminal?.let { main { sessions.close(it.id) } }
            runBlocking { controller.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            File(local.parentFile, "${local.name}-outside.bin").delete()
            File(local.parentFile, "${local.name}-directory-link").delete()
            local.deleteRecursively()
            folder.deleteRecursively()
        }
    }
}

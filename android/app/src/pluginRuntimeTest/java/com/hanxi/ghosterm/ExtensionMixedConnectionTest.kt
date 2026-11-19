package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
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
import java.util.UUID

/** Actual Native group and UI consent; all Executors run on the owned SSH host. */
@RunWith(AndroidJUnit4::class)
class ExtensionMixedConnectionTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun <T> main(block: () -> T): T {
        var result: T? = null; instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 20000
        while (!predicate() && SystemClock.elapsedRealtime() < end) SystemClock.sleep(20)
        assertTrue(label, predicate())
    }
    private fun echo(model: ExtensionPackagesViewModel, text: String, slot: Int) {
        val owner = checkNotNull(model.controller)
        val started = SystemClock.elapsedRealtime()
        val admission = try { runBlocking { owner.requestRuntimeExecutor("fixture.echo",
            JSONObject().put("text", text).put("count", slot).toString().toByteArray(), 10000) } }
        catch (failure: IllegalArgumentException) {
            val observed = runCatching { runBlocking { owner.stepRuntime().info } }.getOrNull()
            throw AssertionError("RPC admission $text/$slot: cached=${main { model.activeRuntime?.info }} observed=$observed issue=${main { model.error }}", failure)
        }
        val handle = checkNotNull(admission.handle)
        await("RPC $text/$slot: ${main { model.error }}") {
            runBlocking { owner.runtimeExecutorRequestResult(handle).second?.confirmed == true }
        }
        val result = checkNotNull(runBlocking { owner.runtimeExecutorRequestResult(handle).second })
        if (result.state != ExtensionSession.RPCState.SUCCEEDED) {
            val elapsed = SystemClock.elapsedRealtime() - started
            val cached = main { model.activeRuntime?.info }
            val observed = runCatching { runBlocking { owner.stepRuntime().info } }
            fail("RPC $text/$slot: ${result.outcome.toString(Charsets.UTF_8)} " +
                "state=${result.state} elapsedMs=$elapsed cached=$cached observed=$observed")
        }
        val value = JSONObject(result.outcome.toString(Charsets.UTF_8)).getJSONObject("value")
        assertEquals(text, value.getString("text")); assertEquals(slot, value.getInt("count"))
        assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { owner.forgetRuntimeExecutorRequest(handle) })
    }
    private fun resource(model: ExtensionPackagesViewModel, slot: Int) {
        echo(model, "resource", slot)
        await("resource $slot: ${main { model.error }}") { main { model.receivedResources.size == 1 } }
        val received = main { model.receivedResources.single() }
        val expected = ByteArray(262144) { ((it + slot * 17) % 251).toByte() }
        assertArrayEquals(expected, received.bytes)
        assertEquals(262144L, received.descriptor.size)
        assertEquals(MessageDigest.getInstance("SHA-256").digest(expected).joinToString("") { "%02x".format(it) }, received.descriptor.sha256)
    }
    private fun approve(instance: ExtensionConnectionModel.Instance) {
        await("consent ${instance.id}") { main { instance.model.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
        assertNull(main { instance.model.activeRuntime })
        ui.onNodeWithTag("plugin-instance-${instance.id}").performScrollTo().performClick()
        ui.onNodeWithText("允许并启动").performClick()
        await("activation ${instance.id}: ${main { instance.model.error }}") {
            main { instance.model.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE }
        }
    }
    @Test fun mixedExecutorsKeepResourcesAndAdmissionFailuresIndependent() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val args = InstrumentationRegistry.getArguments()
        val payload = Socket("127.0.0.1", checkNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((checkNotNull(args.getString("fixtureToken")) + "\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val size = input.readInt(); require(size in 1..1048576)
            JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "mixed-${UUID.randomUUID()}"); check(root.mkdir())
        val files = payload.getJSONObject("packageFiles")
        files.keys().forEach { name ->
            require(!name.startsWith('/') && name.split('/').none { it.isEmpty() || it == "." || it == ".." })
            File(root, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val known = File(context.filesDir, "home/.ssh/known_hosts"); known.parentFile!!.mkdirs()
        val original = known.takeIf(File::exists)?.readBytes()
        val ports = payload.getJSONArray("ports"); val keys = payload.getJSONArray("hostKeys")
        known.writeText((0..1).joinToString("") { "[127.0.0.1]:${ports.getInt(it)} ${keys.getString(it).trimEnd()}\n" })
        val extensions = main { ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java] }
        val sessions = main { ViewModelProvider(ui.activity)[SessionsViewModel::class.java] }
        val owner = checkNotNull(extensions.controller)
        var group: ExtensionConnectionModel? = null
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(payload.getString("packageDigest"), review.digest)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, review.digest) })
            val entry = ExtensionPackagesViewModel.Entry(review, "已批准（本进程）")
            val target = PreparedSSH("mixed-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (payload.getString("route") == "jump") listOf(PreparedSSH("mixed-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val node = ExtensionRelayBootstrap.NodeDependency(payload.getString("node"), payload.getString("nodeVersion"),
                payload.getString("nodeSHA256"), payload.getString("sdk"), payload.getString("sdkSHA256"))
            fun request(slot: Int, runtime: String, signed: Boolean, stale: Boolean = false): ExtensionRelayBootstrap.Request =
                ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString(if (signed) "installed" else "package"),
                    review.digest, review.digest, listOf("fixture.echo"), command = "fixture",
                    workspaceRootDirectory = payload.getString("scratch") + "/workspace-$slot",
                    execution = if (runtime == "portable") null else ExtensionRelayBootstrap.Execution(
                        if (runtime == "node") ExtensionRelayBootstrap.Execution.Runtime.Node(node) else ExtensionRelayBootstrap.Execution.Runtime.Exec,
                        payload.getString("temporary"), true),
                    releaseEvidence = if (signed) ExtensionRelayBootstrap.ReleaseEvidence(payload.getString("release"), payload.getString("policy"),
                        if (stale) "0".repeat(64) else payload.getString("policySHA256"), true) else null, variantID = runtime)
            fun invocation(slot: Int, runtime: String, signed: Boolean, stale: Boolean = false) = ExtensionGroupInvocation(target.alias,
                main { extensions.prepareLaunch(entry, review.commands.single(), request(slot, runtime, signed, stale), null) }, payload.getString("temporary"))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            val runtimes = listOf("portable", "node", "exec", "portable", "node", "exec", "portable", "node")
            main {
                runtimes.forEachIndexed { slot, runtime -> extensions.enqueueGroup(entry, review.commands.single(), target.alias,
                    request(slot, runtime, slot % 2 == 1), null, payload.getString("temporary")) }
                assertNull(extensions.error); assertEquals(8, extensions.pendingGroup.size)
                extensions.launchGroup(sessions) { target }
            }
            await("group created") { main { extensions.connectionGroups.isNotEmpty() } }
            val current = main { extensions.connectionGroups.single() }; group = current
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            val end = SystemClock.elapsedRealtime() + 60000
            while (main { current.busy && current.issue == null } && SystemClock.elapsedRealtime() < end) {
                val prompt = main { sessions.connecting?.auth }
                val identity = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || identity == handled) { SystemClock.sleep(20); continue }; handled = identity
                when (prompt.phase) {
                    SSHAuthPhase.CREDENTIALS -> runBlocking { current.controller.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.HOSTKEY -> fail("Only explicitly pinned owned hosts may be used")
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned mixed SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { current.issue }); assertFalse(main { current.busy })
            val instances = main { current.instances.toList() }; assertEquals(8, instances.size)
            instances.forEach(::approve)
            runBlocking { instances.mapIndexed { slot, instance -> async(Dispatchers.Default) { echo(instance.model, "slot-$slot", slot) } }.awaitAll() }
            instances.forEachIndexed { slot, instance -> resource(instance.model, slot) }
            assertEquals(8, main { instances.map { it.model.receivedResources.single().descriptor.sha256 }.distinct().size })
            val uncertain = instances[3].model
            main {
                val peers = instances.withIndex().filter { it.index != 3 }
                fun checkPeers(stage: String) {
                    peers.forEach { (slot, instance) ->
                        val model = instance.model
                        assertTrue("$stage slot=$slot runtime=${model.activeRuntime?.info} " +
                            "busy=${model.launchBusy} observation=${model.runtimeObservationIssue} issue=${model.error}",
                            model.canInteractWithRuntime)
                    }
                }
                checkPeers("Before observation failure")
                uncertain.runtimeObservationFailed("owned mixed observation failure")
                assertEquals(ExtensionRuntime.Phase.ACTIVE, uncertain.activeRuntime?.info?.phase)
                assertFalse(uncertain.canInteractWithRuntime)
                assertTrue(uncertain.runtimeStateIsUnknown)
                checkPeers("After observation failure")
            }
            val refused = assertThrows(IllegalStateException::class.java) { runBlocking {
                uncertain.requestPluginViewOutcome("fixture.echo", """{"text":"blocked","count":3}""".toByteArray())
            } }
            assertTrue(refused.message?.contains("运行状态未知") == true)
            main { uncertain.stopRuntime() }
            await("explicit stop resumes only cleanup observation") {
                main { uncertain.activeRuntime?.info?.phase == ExtensionRuntime.Phase.STOPPED && !uncertain.launchBusy }
            }
            assertFalse(main { uncertain.runtimeStateIsUnknown })
            assertFalse(main { uncertain.canInteractWithRuntime })
            ui.onNodeWithTag("plugin-instance-close-${instances[3].id}").performScrollTo().performClick()
            assertFalse(main { instances[3].closeRequested })
            ui.onNodeWithTag("plugin-close-cancel").performClick()
            echo(instances[0].model, "close-cancelled", 0)
            ui.onNodeWithTag("plugin-instance-close-${instances[3].id}").performScrollTo().performClick()
            main { extensions.selectGroupInstance(instances[0].model) }
            ui.onNodeWithTag("plugin-close-confirm").performClick()
            await("fourth released") { main { instances[3].closed } }
            val invalid = invocation(3, "node", true, true)
            val rejected = runBlocking { withContext(Dispatchers.Main) { current.add(invalid) } }
            assertNull(rejected); assertEquals(7, main { current.instances.count { !it.closed } })
            main { current.instances.toList() }.forEachIndexed { slot, instance -> if (!main { instance.closed }) echo(instance.model, "after-rejection-$slot", slot) }
            main { extensions.enqueueGroup(entry, review.commands.single(), target.alias, request(3, "node", true), null, payload.getString("temporary")) }
            val pending = main { extensions.pendingGroup.single() }
            ui.onNodeWithTag("plugin-add-${current.id}-${pending.id}").performScrollTo().performClick()
            await("online Node consent") { main { !extensions.groupSubmissionBusy && current.instances.lastOrNull()?.id == pending.id } }
            val replacement = main { current.instances.last() }; approve(replacement); resource(replacement.model, 3)
            main { current.instances.toList().forEach(current::requestCloseInstance) }
            await("all closed") { main { current.instances.all { it.closed } } }
            assertFalse(main { current.closed }); assertTrue(main { current.canAdd })
            assertEquals(0, runBlocking { current.controller.stepMultiplexConnection().info.instances })
            main { extensions.enqueueGroup(entry, review.commands.single(), target.alias, request(0, "exec", true), null, payload.getString("temporary")) }
            val finalPending = main { extensions.pendingGroup.single() }
            ui.onNodeWithTag("plugin-add-${current.id}-${finalPending.id}").performScrollTo().performClick()
            await("empty connection Exec consent") { main { current.instances.lastOrNull()?.id == finalPending.id } }
            val final = main { current.instances.last() }; approve(final); resource(final.model, 0)
            ui.onNodeWithTag("plugin-group-close-${current.id}").performScrollTo().performClick()
            assertFalse(main { final.closeRequested }); assertFalse(main { current.closed })
            ui.onNodeWithTag("plugin-close-confirm").performClick()
            await("group release: ${main { current.issue }}") { main { current.closed } }
            assertFalse(main { current.cleanupPending }); assertTrue(main { current.state?.info?.released == true })
            assertTrue(main { current.state?.transport?.remoteEof == true })
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.resolve("fixture", review.id, review.digest).status })
        } finally {
            group?.let { runBlocking { withContext(Dispatchers.Main) { it.close() } } }
            runBlocking { owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

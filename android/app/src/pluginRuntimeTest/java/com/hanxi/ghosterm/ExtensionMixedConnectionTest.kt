package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import android.os.Process
import android.system.OsConstants
import android.os.SystemClock
import android.os.ParcelFileDescriptor
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
        val end = SystemClock.elapsedRealtime() + 20_000
        while (!predicate() && SystemClock.elapsedRealtime() < end) SystemClock.sleep(20)
        assertTrue(label, predicate())
    }
    private fun echo(model: ExtensionPackagesViewModel, text: String, slot: Int, stage: String = "initial") {
        val owner = checkNotNull(model.controller)
        val started = SystemClock.elapsedRealtime()
        val label = "RPC $text/$slot:"
        // Publication has its own 10 s limit. Match the UI's outer RPC budget
        // for this call so the executor can still report a publication failure.
        val rpcTimeoutMs = if (text == "resource") 30_000 else 10_000
        val admission = try { runBlocking { owner.requestRuntimeExecutor("fixture.echo",
            JSONObject().put("text", text).put("count", slot).toString().toByteArray(), rpcTimeoutMs) } }
        catch (failure: IllegalArgumentException) {
            val observed = runCatching { runBlocking { owner.stepRuntime().info } }.getOrNull()
            throw AssertionError("RPC admission $text/$slot: cached=${main { model.activeRuntime?.info }} observed=$observed issue=${main { model.error }}", failure)
        }
        val handle = checkNotNull(admission.handle)
        var observation = runBlocking { owner.runtimeExecutorRequestResult(handle) }
        val deadline = started + rpcTimeoutMs + 5_000L
        while (observation.first == ExtensionSession.RPCStatus.OK &&
            observation.second?.state in setOf(ExtensionSession.RPCState.QUEUED, ExtensionSession.RPCState.PENDING) &&
            SystemClock.elapsedRealtime() < deadline) {
            SystemClock.sleep(20)
            observation = runBlocking { owner.runtimeExecutorRequestResult(handle) }
        }
        val (status, result) = observation
        if (status != ExtensionSession.RPCStatus.OK ||
            result?.state != ExtensionSession.RPCState.SUCCEEDED || !result.confirmed) {
            val elapsed = SystemClock.elapsedRealtime() - started
            val cached = main { model.activeRuntime?.info }
            val observed = runCatching { runBlocking { owner.stepRuntime().info } }
            throw AssertionError("$label ${result?.outcome?.toString(Charsets.UTF_8)} " +
                "stage=$stage status=$status state=${result?.state} confirmed=${result?.confirmed} " +
                "elapsedMs=$elapsed cached=$cached observed=$observed")
        }
        val value = JSONObject(result.outcome.toString(Charsets.UTF_8)).getJSONObject("value")
        assertEquals("$label stage=$stage elapsedMs=${SystemClock.elapsedRealtime() - started}",
            text, value.getString("text"))
        assertEquals(slot, value.getInt("count"))
        assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { owner.forgetRuntimeExecutorRequest(handle) })
    }
    private fun resource(model: ExtensionPackagesViewModel, slot: Int, stage: String = "initial") {
        echo(model, "resource", slot, stage)
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
        val fixturePort = checkNotNull(args.getString("fixturePort")).toInt()
        val fixtureToken = checkNotNull(args.getString("fixtureToken"))
        fun transportFault(mode: String) = Socket("127.0.0.1", fixturePort).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write("$fixtureToken:$mode\n".toByteArray())
            assertEquals("owned SSH fault $mode", "OK", socket.getInputStream().bufferedReader().readLine())
        }
        val payload = Socket("127.0.0.1", fixturePort).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write("$fixtureToken\n".toByteArray())
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
        val revocation = payload.optJSONObject("revocation")
        fun bytes(bundle: JSONObject, key: String): ByteArray = Base64.decode(bundle.getString(key), Base64.DEFAULT)
        fun updatePolicy(bundle: JSONObject) = checkNotNull(PluginChannelRefresh.configured(context)).store.refresh(
            bytes(bundle, "statement"), bytes(bundle, "signature"), bytes(bundle, "policy"),
            (System.currentTimeMillis() / 1000).toULong())
        var group: ExtensionConnectionModel? = null
        try {
            val review = runBlocking {
                if (revocation == null) owner.review(root) else {
                    check(File(context.filesDir, "plugin-channel-policy").mkdir())
                    assertEquals(1uL, updatePolicy(revocation.getJSONObject("initial")).generation)
                    val archive = File(root, "fixture.zip").apply { writeBytes(bytes(revocation, "artifact")) }
                    val descriptor = bytes(revocation, "descriptor")
                    val signature = bytes(revocation, "signature")
                    val installed = ParcelFileDescriptor.open(archive, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                        val expected = owner.inspectConfiguredChannelArchive(fd, descriptor, signature)
                        owner.installConfiguredChannelArchive(fd, descriptor, signature, expected)
                    }
                    val revision = owner.installedVersions().selection?.revision
                    val preview = owner.previewInstalledVersion(UUID.randomUUID().toString(), installed.publication.releaseID, revision, byteArrayOf())
                    checkNotNull(owner.confirmInstalledVersion(preview.id, true).review)
                }
            }
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
            val runtimes = if (revocation == null) listOf("portable", "node", "exec", "portable", "node", "exec", "portable", "node")
                else listOf("portable")
            main {
                runtimes.forEachIndexed { slot, runtime -> extensions.enqueueGroup(entry, review.commands.single(), target.alias,
                    request(slot, runtime, revocation != null || slot % 2 == 1), null, payload.getString("temporary")) }
                assertNull(extensions.error); assertEquals(runtimes.size, extensions.pendingGroup.size)
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
            val instances = main { current.instances.toList() }; assertEquals(runtimes.size, instances.size)
            if (revocation != null) {
                val instance = instances.single()
                val model = instance.model
                val active = payload.getBoolean("activeRevocation")
                val releaseID = runBlocking { checkNotNull(owner.installedVersions().selection).choices.values.single().releaseID }
                val releases = File(context.filesDir, "plugin-releases")
                fun leaseStatus() = LocalPublicationFile.directory(releases, releaseID).use { PluginInstallFiles.lock(it.fd) }
                await("managed Session consent") {
                    main { model.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT && !model.launchBusy }
                }
                if (active) {
                    approve(instance)
                    resource(model, 0, stage = "before publisher revocation")
                    assertEquals(OsConstants.EAGAIN, leaseStatus())
                    ui.onNodeWithTag("plugin-channel-stop").assertDoesNotExist()
                }
                val materials = revocation.getJSONObject("revoked")
                val writer = Uri.parse("content://${context.packageName}.test-channel-writer")
                val written = checkNotNull(context.contentResolver.call(
                    writer, "commit", null,
                    Bundle().apply {
                        for (name in listOf("statement", "signature", "policy")) putByteArray(name, bytes(materials, name))
                    }))
                assertTrue("channel writer must run in a different process",
                    written.getInt("pid") > 0 && written.getInt("pid") != Process.myPid())
                assertEquals("2", written.getString("generation"))
                val revoked = runBlocking { owner.cachedChannelPolicy(recordObservation = false) }
                assertEquals(2uL, revoked.generation)
                assertEquals(written.getString("statementSHA256"), revoked.statementSHA256)
                assertEquals(1, runBlocking { owner.runningInstancesWithRevokedPublisher(revoked) })
                if (active) {
                    ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-channel-stop").fetchSemanticsNodes().isNotEmpty() }
                    ui.onNodeWithTag("plugin-channel-affected").assertTextContains("1 个", substring = true)
                    assertTrue(main { model.canInteractWithRuntime })
                    echo(model, "still active", 0, stage = "after publisher revocation")
                    assertEquals(OsConstants.EAGAIN, leaseStatus())
                    if (payload.getBoolean("disconnectOnStop")) {
                        transportFault("blackhole")
                        ui.onNodeWithTag("plugin-channel-stop").performScrollTo().performClick()
                        ui.waitUntil(15000) {
                            ui.onAllNodes(hasTestTag("plugin-channel-refresh-status") and
                                hasText("已向 1 个实例发出停止请求", substring = true)).fetchSemanticsNodes().isNotEmpty()
                        }
                        transportFault("reset")
                        await("owned SSH transport reset not observed") { main { current.state?.info?.error?.let { it != 0 } == true } }
                        assertFalse(main { model.canInteractWithRuntime })
                        assertEquals(OsConstants.EAGAIN, leaseStatus())
                        assertTrue(runBlocking { withContext(Dispatchers.Main) { current.close() } })
                        assertTrue(main { current.state?.info?.released == true })
                        assertFalse("TCP reset must not establish remote exit", checkNotNull(main { current.state?.transport }).remoteEof)
                        assertTrue(main { current.issue?.contains("执行端退出未确认") == true })
                        assertEquals(0, leaseStatus())
                        return
                    }
                    ui.onNodeWithTag("plugin-channel-stop").performScrollTo()
                    checkNotNull(context.contentResolver.call(writer, "hold-cache-lock", null, null))
                    try {
                        ui.onNodeWithTag("plugin-channel-stop").performClick()
                        ui.waitUntil(15000) {
                            ui.onAllNodesWithTag("plugin-channel-refresh-status").fetchSemanticsNodes().isNotEmpty()
                        }
                        ui.onNodeWithTag("plugin-channel-refresh-status")
                            .assertTextContains("已向 0 个实例发出停止请求", substring = true)
                            .assertTextContains("1 个实例未能完成核对或发出请求", substring = true)
                            .assertTextContains("安装目录正由另一项操作使用", substring = true)
                        assertTrue(main { model.canInteractWithRuntime })
                        echo(model, "stop unavailable", 0, stage = "channel cache busy")
                        assertEquals(OsConstants.EAGAIN, leaseStatus())
                    } finally {
                        checkNotNull(context.contentResolver.call(writer, "release-cache-lock", null, null))
                    }
                    ui.waitUntil(15000) {
                        ui.onAllNodes(hasTestTag("plugin-channel-stop") and isEnabled()).fetchSemanticsNodes().isNotEmpty()
                    }
                    ui.onNodeWithTag("plugin-channel-stop").performScrollTo().performClick()
                    ui.waitUntil(15000) {
                        ui.onAllNodes(hasTestTag("plugin-channel-refresh-status") and
                            hasText("已向 1 个实例发出停止请求", substring = true)).fetchSemanticsNodes().isNotEmpty()
                    }
                    ui.onNodeWithTag("plugin-channel-refresh-status").assertTextContains("已向 1 个实例发出停止请求", substring = true)
                    await("revoked Runtime did not stop") {
                        main { !model.launchBusy && model.activeRuntime?.info?.phase == ExtensionRuntime.Phase.STOPPED }
                    }
                    assertEquals(OsConstants.EAGAIN, leaseStatus())
                    ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
                    await("revoked Runtime did not release") { main { !model.launchBusy && model.activeRuntime == null } }
                    assertEquals(0, leaseStatus())
                    assertEquals(0, runBlocking { owner.runningInstancesWithRevokedPublisher(revoked) })
                    ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-channel-stop").fetchSemanticsNodes().isEmpty() }
                    return
                }
                ui.onNodeWithTag("plugin-instance-${instance.id}").performScrollTo().performClick()
                ui.onNodeWithText("允许并启动").performClick()
                await("revoked consent did not finish") { main { !model.launchBusy } }
                assertEquals("revoked consent must not advance the Native Session", ExtensionSession.Phase.CONSENT,
                    main { model.activeSession?.info?.phase })
                assertTrue(main { model.error?.contains("撤销") == true })
                assertNull(main { model.activeRuntime })
                assertTrue(main { model.receivedResources.isEmpty() })
                val stop = runBlocking { withContext(Dispatchers.Main) { extensions.requestStopInstancesWithRevokedPublisher() } }
                assertEquals("revocation stop must request cancellation of pending consent", 1, stop.requested)
                assertTrue(stop.failures.toString(), stop.failures.isEmpty())
                await("revoked Session did not stop") {
                    main { !model.launchBusy && model.activeSession?.info?.phase in
                        setOf(ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED) }
                }
                main { model.releaseFinished() }
                await("revoked Session did not release") { main { !model.launchBusy && model.activeSession == null } }
                assertEquals(0, runBlocking { owner.runningInstancesWithRevokedPublisher(revoked) })
                return
            }
            instances.forEach(::approve)
            runBlocking { instances.mapIndexed { slot, instance -> async(Dispatchers.Default) { echo(instance.model, "slot-$slot", slot) } }.awaitAll() }
            // Exercise all scopes under the same external-executor lifetime
            // instead of making later scopes wait for earlier publications.
            runBlocking {
                instances.mapIndexed { slot, instance ->
                    async(Dispatchers.Default) { resource(instance.model, slot) }
                }.awaitAll()
            }
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
            val replacement = main { current.instances.last() }; approve(replacement)
            resource(replacement.model, 3, stage = "online replacement")
            main { current.instances.toList().forEach(current::requestCloseInstance) }
            await("all closed") { main { current.instances.all { it.closed } } }
            assertFalse(main { current.closed }); assertTrue(main { current.canAdd })
            assertEquals(0, runBlocking { current.controller.stepMultiplexConnection().info.instances })
            main { extensions.enqueueGroup(entry, review.commands.single(), target.alias, request(0, "exec", true), null, payload.getString("temporary")) }
            val finalPending = main { extensions.pendingGroup.single() }
            ui.onNodeWithTag("plugin-add-${current.id}-${finalPending.id}").performScrollTo().performClick()
            await("empty connection Exec consent") { main { current.instances.lastOrNull()?.id == finalPending.id } }
            val final = main { current.instances.last() }; approve(final)
            resource(final.model, 0, stage = "empty connection")
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

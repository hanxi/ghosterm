package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.CompletableDeferred
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
import java.util.UUID

/** Production shared directory, eight Clients/Executors, real consent and switch UI. */
@RunWith(AndroidJUnit4::class)
class ExtensionConnectionTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun <T> main(block: () -> T): T {
        var result: T? = null; instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String, test: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 20000
        while (!test() && SystemClock.elapsedRealtime() < end) SystemClock.sleep(20)
        assertTrue(label, test())
    }
    @Test fun eightInstancesShareSSHAndCloseIndependently() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val args = InstrumentationRegistry.getArguments()
        val payload = Socket("127.0.0.1", checkNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((checkNotNull(args.getString("fixtureToken")) + "\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val size = input.readInt(); require(size in 1..1048576)
            JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "mux-${UUID.randomUUID()}"); check(root.mkdir())
        val files = payload.getJSONObject("packageFiles")
        files.keys().forEach { name ->
            require(!name.startsWith('/') && name.split('/').none { it.isEmpty() || it == ".." })
            File(root, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val known = File(context.filesDir, "home/.ssh/known_hosts"); known.parentFile!!.mkdirs()
        val original = known.takeIf(File::exists)?.readBytes()
        val ports = payload.getJSONArray("ports"); val hostKeys = payload.getJSONArray("hostKeys")
        known.writeText((0..1).joinToString("") { "[127.0.0.1]:${ports.getInt(it)} ${hostKeys.getString(it)}" })
        val extensions = main { ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java] }
        val sessions = main { ViewModelProvider(ui.activity)[SessionsViewModel::class.java] }
        val owner = checkNotNull(extensions.controller)
        var group: ExtensionConnectionModel? = null
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, review.digest) })
            val target = PreparedSSH("mux-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (payload.getString("route") == "jump") listOf(PreparedSSH("mux-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            val targetPreflight = CompletableDeferred<Unit>()
            main {
                repeat(8) { index ->
                    val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), review.digest, review.digest,
                        emptyList(), command = "table", arguments = listOf("--count", "${index + 1}"))
                    extensions.enqueueGroup(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(),
                        target.alias, request, null, payload.getString("temporary"))
                }
                assertNull(extensions.error); assertEquals(8, extensions.pendingGroup.size)
                extensions.launchGroup(sessions) {
                    targetPreflight.await()
                    target.copy(alias = "different-target")
                }
                assertTrue(extensions.groupSubmissionBusy)
                extensions.removePendingGroupItem(extensions.pendingGroup.first())
                assertEquals(8, extensions.pendingGroup.size)
                assertTrue(extensions.connectionGroups.isEmpty())
                targetPreflight.complete(Unit)
            }
            await("mismatched group released") {
                main { extensions.connectionGroups.firstOrNull()?.closed == true && !extensions.groupSubmissionBusy }
            }
            main {
                assertEquals(8, extensions.pendingGroup.size)
                assertNull(extensions.selectedGroupInstance)
                extensions.launchGroup(sessions) { target }
            }
            await("group created") { main { extensions.connectionGroups.size == 2 } }
            val current = main { extensions.connectionGroups.last() }; group = current
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            val end = SystemClock.elapsedRealtime() + 60000
            while (main { current.busy && current.issue == null } && SystemClock.elapsedRealtime() < end) {
                val prompt = main { sessions.connecting?.auth }
                val identity = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || identity == handled) { SystemClock.sleep(20); continue }; handled = identity
                when (prompt.phase) {
                    SSHAuthPhase.CREDENTIALS -> runBlocking { current.controller.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.HOSTKEY -> fail("Only explicitly pinned owned hosts may be used")
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned mux SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { current.issue }); assertFalse(main { current.busy })
            await("confirmed group consumes pending items") { main { extensions.pendingGroup.isEmpty() } }
            assertEquals(8, main { current.instances.size }); assertEquals(8, main { current.instances.map { it.id }.distinct().size })
            val instances = main { current.instances.toList() }
            for ((index, instance) in instances.withIndex()) {
                await("consent $index") { main { instance.model.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
                if (index > 0) ui.onNodeWithTag("plugin-instance-${instance.id}").performScrollTo().performClick()
                ui.onNodeWithText("允许并启动").performClick()
                await("active $index: ${main { instance.model.error }}") { main { instance.model.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
                val table = main { instance.model.table }; assertNotNull(table)
                await("table idle $index") { main { !table!!.state.value.busy } }
                val request = main { table!!.load() }; assertNotNull(request); runBlocking { request?.join() }
                assertNull(main { table!!.state.value.issue }); assertEquals(index + 1, main { table!!.state.value.descriptor?.total })
            }
            ui.onNodeWithTag("plugin-instance-close-${instances[3].id}").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-close-confirm").assertIsDisplayed()
            assertFalse(main { instances[3].closeRequested }); assertFalse(main { instances[3].closed })
            ui.onNodeWithTag("plugin-close-cancel").performClick()
            assertEquals(8, runBlocking { current.controller.stepMultiplexConnection().info.instances })
            ui.onNodeWithTag("plugin-instance-close-${instances[3].id}").performScrollTo().performClick()
            main { extensions.selectGroupInstance(instances[0].model) }
            ui.onNodeWithTag("plugin-close-confirm").performClick()
            await("fourth instance released") { main { instances[3].closed } }
            for ((index, instance) in instances.withIndex()) if (index != 3) {
                val table = main { instance.model.table!! }
                await("sibling idle $index") { main { !table.state.value.busy } }
                val request = main { table.load() }; assertNotNull(request); runBlocking { request?.join() }
                assertNull(main { table.state.value.issue }); assertEquals(index + 1, main { table.state.value.descriptor?.total })
            }
            assertEquals(7, runBlocking { current.controller.stepMultiplexConnection().info.instances })
            assertNull(main { extensions.activeSession }) // Default singleton never owns a group scope.
            val replacementRequest = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), review.digest, review.digest,
                emptyList(), command = "table", arguments = listOf("--count", "33"))
            main { extensions.enqueueGroup(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(),
                target.alias, replacementRequest, null, payload.getString("temporary")) }
            val candidate = main { extensions.pendingGroup.single() }
            ui.onNodeWithTag("plugin-add-${current.id}-${candidate.id}").performScrollTo().performClick()
            await("online consent") { main { !extensions.groupSubmissionBusy && current.instances.lastOrNull()?.id == candidate.id && current.instances.last().model.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            val replacement = main { current.instances.last() }
            assertEquals(9L, main { replacement.model.activeSession!!.info.epoch })
            ui.onNodeWithText("允许并启动").performClick()
            await("online runtime") { main { replacement.model.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
            val freshTable = main { replacement.model.table!! }
            await("online table idle") { main { !freshTable.state.value.busy } }
            runBlocking { main { freshTable.load() }?.join() }
            assertEquals(33, main { freshTable.state.value.descriptor?.total }); assertNull(main { freshTable.state.value.issue })
            for (instance in main { current.instances.toList() }) {
                val table = main { instance.model.table!! }; await("sibling stable after online add") { main { !table.state.value.busy } }
                runBlocking { main { table.load() }?.join() }; assertNull(main { table.state.value.issue })
            }
            main { current.instances.toList().forEach { current.requestCloseInstance(it) } }
            await("all instances closed") { main { current.instances.all { it.closed } } }
            assertFalse(main { current.closed }); assertTrue(main { current.canAdd })
            assertEquals(0, runBlocking { current.controller.stepMultiplexConnection().info.instances })
            main { extensions.enqueueGroup(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(),
                target.alias, replacementRequest.copy(arguments = listOf("--count", "34")), null, payload.getString("temporary")) }
            val lastCandidate = main { extensions.pendingGroup.single() }
            ui.onNodeWithTag("plugin-add-${current.id}-${lastCandidate.id}").performScrollTo().performClick()
            await("empty connection reuse consent") { main { current.instances.lastOrNull()?.id == lastCandidate.id && current.instances.last().model.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            ui.onNodeWithText("允许并启动").performClick()
            val finalModel = main { current.instances.last().model }
            await("empty connection reuse active") { main { finalModel.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
            val finalTable = main { finalModel.table!! }; await("final table idle") { main { !finalTable.state.value.busy } }
            runBlocking { main { finalTable.load() }?.join() }; assertEquals(34, main { finalTable.state.value.descriptor?.total })
            ui.onNodeWithTag("plugin-group-close-${current.id}").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-close-confirm").assertIsDisplayed()
            assertFalse(main { current.closed }); assertFalse(main { current.instances.last().closeRequested })
            ui.onNodeWithTag("plugin-close-cancel").performClick()
            runBlocking { main { finalTable.load() }?.join() }; assertEquals(34, main { finalTable.state.value.descriptor?.total })
            ui.onNodeWithTag("plugin-group-close-${current.id}").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-close-confirm").performClick()
            await("group release: ${main { current.issue }}") { main { current.closed } }
            assertFalse(main { current.cleanupPending }); assertTrue(main { current.state?.info?.released == true })
            assertTrue("Remote cleanup must precede physical close", main { current.state?.transport?.remoteEof == true })
            assertTrue(main { current.instances.all { it.closed } })
            val sibling = runBlocking { owner.makeInvocationController() }
            runBlocking { sibling.close() }
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.resolve("table", review.id, review.digest).status })
        } finally {
            group?.let { current -> runBlocking { withContext(Dispatchers.Main) { current.close() } } }
            runBlocking { owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

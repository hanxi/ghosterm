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
class PluginViewsSSHAppTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private var diagnostic: () -> String = { "" }
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
        assertTrue(label + " " + diagnostic(), check())
    }
    @Test fun registeredViewsThroughViewModelAndNativeControls() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture(); val jump = payload.getString("route") == "jump"
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "views-${java.util.UUID.randomUUID()}"); check(root.mkdir())
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
        var captured: PluginViewsModel? = null
        diagnostic = { main {
            "phase=${extensions.activeRuntime?.info?.phase}; error=${extensions.error}; valid=${captured?.state?.value?.valid}; " +
                "views=${captured?.state?.value?.items?.map { Triple(it.registration.viewID, it.closing, it.closeIssue) }}"
        } }
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(listOf("table", "diff", "task"), review.commands.single().views)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, hash) })
            val target = PreparedSSH("views-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("views-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val methods = listOf("alpha.describe", "alpha.page", "beta.describe", "beta.page", "changes.describe", "changes.page", "work.describe", "work.read", "work.cancel", "views.reopen")
            val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), hash, hash,
                methods, command = "views demo", hostMethods = listOf("views.client.open"))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            main { extensions.launch(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), review.commands.single(), target, request, sessions) }
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            val deadline = SystemClock.elapsedRealtime() + 60000
            while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < deadline) {
                val prompt = main { sessions.connecting?.auth }; val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || id == handled) { SystemClock.sleep(20); continue }; handled = id
                when (prompt.phase) {
                    SSHAuthPhase.CREDENTIALS -> runBlocking { owner.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.HOSTKEY -> fail("Only explicitly pinned owned hosts may be used")
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned views SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { extensions.error })
            await("Views Session consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(methods, main { extensions.activeSession?.methods })
            assertEquals(setOf("session.rpc", "ui.table", "ui.diff", "ui.task", "ui.views"), main { extensions.activeSession?.requiredCapabilities?.toSet() })
            ui.onNodeWithText("允许并启动").performClick()
            await("Client activation reaches an observed phase") { main { extensions.activeRuntime?.info?.phase?.let {
                it !in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING) } == true } }
            val views = main { extensions.registeredViews }
            assertNotNull("Native multi-view registration must be enabled before activation", views); if (views == null) return
            captured = views
            await("Four independent Native views") { main { views.state.value.items.size == 4 } }
            assertTrue(main { extensions.table == null && extensions.diff == null && extensions.task == null })
            fun item(id: String) = main { views.state.value.items.single { it.registration.viewID == id } }
            val alphaItem = item("alpha"); val alpha = (alphaItem.content as PluginViewItem.Content.Table).model
            val beta = (item("beta").content as PluginViewItem.Content.Table).model
            val diff = (item("changes").content as PluginViewItem.Content.Diff).model
            val task = (item("progress").content as PluginViewItem.Content.Task).model
            runBlocking { kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main.immediate) {
                alpha.load()?.join(); beta.load()?.join(); diff.load()?.join(); task.load()?.join()
            } }
            await("All descriptors and pages") { main { alpha.state.value.page != null && beta.state.value.page != null && diff.state.value.page != null && task.state.value.snapshot != null } }
            assertEquals(listOf("before", "after"), main { diff.state.value.page?.rows?.map { it.text } })
            var refused = false
            try { runBlocking { owner.closeRuntimeView("foreign-runtime", alphaItem.handle) } } catch (_: IllegalStateException) { refused = true }
            assertTrue("Native close must reject another Runtime UUID", refused); if (!refused) return
            ui.onAllNodesWithTag("plugin-view-close").assertCountEquals(4)
            ui.onAllNodesWithTag("plugin-table-next")[0].performScrollTo().performClick()
            await("Alpha second page") { main { alpha.state.value.page?.offset == 16 } }; assertEquals(0, main { beta.state.value.page?.offset })
            ui.onNodeWithContentDescription("关闭视图 alpha，不取消任务").performScrollTo().performClick()
            await("Native Alpha close") { main { views.state.value.items.size == 3 } }
            assertTrue(main { !alpha.state.value.valid && views.state.value.items.any { (it.content as? PluginViewItem.Content.Table)?.model === beta } })
            fun rpc(method: String, input: String) = runBlocking { kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main.immediate) {
                extensions.requestPluginViewOutcome(method, input.toByteArray()).toString(Charsets.UTF_8)
            } }
            assertTrue(JSONObject(rpc("views.reopen", """{"viewId":"alpha"}""")).getJSONObject("value").getBoolean("opened"))
            await("Client reopens Alpha") { main { views.state.value.items.any { it.registration.viewID == "alpha" && it.handle != alphaItem.handle } } }
            val freshAlpha = (item("alpha").content as PluginViewItem.Content.Table).model
            ui.onAllNodesWithTag("plugin-view-close").assertCountEquals(4)
            await("Reopened table finishes changing the layout") {
                ui.mainClock.advanceTimeByFrame()
                main { freshAlpha.state.value.page != null && !freshAlpha.state.value.busy }
            }
            ui.waitForIdle()
            ui.onNodeWithContentDescription("关闭视图 progress，不取消任务").performScrollTo().performClick()
            await("Task widget close") { main { views.state.value.items.size == 3 } }
            assertEquals("running", JSONObject(rpc("work.read", """{"viewId":"progress","revision":"1"}""")).getJSONObject("value").getString("phase"))
            rpc("views.reopen", """{"viewId":"progress"}""")
            await("Task widget reopens") { main { views.state.value.items.size == 4 } }
            val reopened = (item("progress").content as PluginViewItem.Content.Task).model
            ui.onNodeWithTag("plugin-task").assertExists(); await("Reopened task accepts explicit cancel") { main { reopened.canCancel } }
            ui.waitForIdle()
            ui.onNodeWithTag("plugin-task-cancel").performScrollTo().performClick()
            await("Actual cancel confirmation") { ui.mainClock.advanceTimeBy(600); main { reopened.state.value.snapshot?.phase == PluginTaskSnapshot.Phase.CANCELLED } }
            assertEquals(PluginTaskSnapshot.Result.Cancelled("none"), main { reopened.state.value.snapshot?.result })
            main { extensions.stopRuntime() }
            await("Views Runtime stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.ACTIVE,
                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.CREATED) } }
            assertTrue(main { !views.state.value.valid && views.state.value.items.isEmpty() })
            ui.onNodeWithText("释放插件资源").performScrollTo().assertIsEnabled().performClick()
            await("Views owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }; assertNull(main { extensions.error })
        } finally {
            main { captured?.invalidate(); if (!extensions.launchBusy) extensions.stopRuntime() }
            runBlocking { owner.requestStopRuntime(); owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

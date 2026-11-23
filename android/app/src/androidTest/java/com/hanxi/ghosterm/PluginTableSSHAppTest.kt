package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.compose.ui.test.*
import androidx.compose.ui.semantics.SemanticsActions
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
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.DataInputStream
import java.io.File
import java.net.Socket
import java.security.MessageDigest

/** Actual production ViewModel, consent/paging UI and shared official Executor. */
@RunWith(AndroidJUnit4::class)
class PluginTableSSHAppTest {
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
    @Test fun officialTableThroughViewModelConsentAndNativePaging() {
        assumeTrue(InstrumentationRegistry.getArguments().containsKey("fixturePort"))
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        val payload = fixture(); val count = payload.getInt("count"); val jump = payload.getString("route") == "jump"
        val logs = payload.optString("sample") == "logs"
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "table-${java.util.UUID.randomUUID()}"); check(root.mkdir())
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
        var capturedTable: PluginTableModel? = null
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(listOf("table"), review.commands.single().views)
            assertEquals(!logs, review.commands.single().togglePanel)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, hash) })
            val target = PreparedSSH("table-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("table-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val project = payload.optString("project").takeIf { it.isNotEmpty() }
            val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), hash, hash,
                emptyList(), command = if (logs) "logs" else "table",
                arguments = project?.let { listOf("--project", it) } ?: listOf("--count", "$count"))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            runBlocking {
                withContext(Dispatchers.Main) {
                    val entry = ExtensionPackagesViewModel.Entry(review, "已批准（本进程）")
                    val command = review.commands.single()
                    val invalid = request.copy(arguments = listOf("--count", "invalid"))
                    val rejected = runCatching { extensions.submitLaunch(entry, command, target, invalid, sessions, null) }
                    assertTrue(rejected.isFailure)
                    assertNull(sessions.connecting)
                    assertNull(extensions.activeSession)
                    assertFalse(extensions.launchBusy)
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
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned table SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { extensions.error })
            await("Table Session consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(listOf("session.rpc", "ui.table"), main { extensions.activeSession?.requiredCapabilities })
            val methods = if (logs) PluginTableModel.methods + PluginTableModel.searchMethod else PluginTableModel.methods
            assertEquals(methods, main { extensions.activeSession?.methods })
            if (main { extensions.activeSession?.methods } != methods) return
            ui.onNodeWithText("允许并启动").performClick()
            await("Table Client activation") { main { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
            val table = main { extensions.table }; assertNotNull("Native table contribution must be wired", table)
            if (table == null) return
            capturedTable = table
            // Drive the test frame clock through the production contribution;
            // polling the worker alone does not compose its LaunchedEffect.
            ui.onNodeWithTag("plugin-table").assertExists()
            await("First Native page: ${main { extensions.error }}") { main { table.state.value.page != null || table.state.value.issue != null } }
            assertNull(main { table.state.value.issue })
            assertEquals(if (logs) 4096 else if (project != null) 2 else count, main { table.state.value.descriptor?.total })
            if (logs) {
                assertTrue(table.supportsSearch)
                assertEquals("905", main { table.state.value.page?.rows?.first()?.id })
                ui.onNodeWithTag("plugin-table-search-query").performScrollTo().performTextInput("record 1100")
                ui.onNodeWithTag("plugin-table-search").assertIsEnabled().performSemanticsAction(SemanticsActions.OnClick)
                await("Empty search page") { main { table.state.value.searchPage?.cursor == 1032L || table.state.value.issue != null } }
                assertNull(main { table.state.value.issue })
                assertEquals(emptyList<PluginTableRow>(), main { table.state.value.searchPage?.rows })
                assertTrue(main { table.state.value.searchTruncated })
                ui.onNodeWithTag("plugin-table-search-more").assertIsEnabled().performSemanticsAction(SemanticsActions.OnClick)
                await("Matching search page") { main { table.state.value.searchPage?.cursor == 1160L || table.state.value.issue != null } }
                assertNull(main { table.state.value.issue })
                ui.onNodeWithTag("plugin-table-cell-1100-message").assertTextEquals("INFO worker 4 completed sample record 1100")
                ui.onNodeWithTag("plugin-table-search-clear").assertIsEnabled().performSemanticsAction(SemanticsActions.OnClick)
                await("Browsing restored") { main { table.state.value.searchQuery == null && table.state.value.page != null } }
                assertEquals("905", main { table.state.value.page?.rows?.first()?.id })
            } else if (project != null) {
                val (row, title) = when (project) {
                    "sample-a" -> "sample-a-review" to "Review release notes"
                    "sample-b" -> "sample-b-inventory" to "Update inventory"
                    else -> error("Unknown project fixture")
                }
                assertEquals("Project tasks · $project", main { table.state.value.descriptor?.title })
                assertEquals(row, main { table.state.value.page?.rows?.first()?.id })
                ui.onNodeWithTag("plugin-table-cell-$row-task").performScrollTo().assertTextEquals(title)
            } else if (count > 0) {
                ui.onNodeWithTag("plugin-table-next").performScrollTo().performClick()
                await("Second Native page") { main { table.state.value.page?.offset == 16 } }
                ui.onNodeWithTag("plugin-table-cell-17-n").performScrollTo().assertTextEquals("17")
                ui.onNodeWithTag("plugin-table-last").performScrollTo().performClick()
                await("Last Native page") { main { table.state.value.page?.offset == 999984 } }
                ui.onNodeWithTag("plugin-table-cell-1000000-cube").performScrollTo().assertTextEquals("1000000000000000000")
            } else ui.onNodeWithTag("plugin-table-empty").performScrollTo().assertTextEquals("空表")
            main { extensions.stopRuntime() }
            await("Table Runtime stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.ACTIVE,
                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.CREATED) } }
            assertFalse(main { table.state.value.valid }); assertNull(main { table.state.value.page })
            ui.onNodeWithText("释放插件资源").performScrollTo().assertIsEnabled().performClick()
            await("Table owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }
            assertNull(main { extensions.error })
        } finally {
            main { capturedTable?.invalidate(); if (!extensions.launchBusy) extensions.stopRuntime() }
            runBlocking { owner.requestStopRuntime(); owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

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
class PluginDiffSSHAppTest {
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
    @Test fun officialDiffThroughViewModelConsentAndNativePaging() {
        assertTrue(BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture(); val total = payload.getInt("total");
        val before = payload.getString("before"); val after = payload.getString("after"); val jump = payload.getString("route") == "jump"
        val context = instrumentation.targetContext
        val root = File(context.cacheDir, "diff-${java.util.UUID.randomUUID()}"); check(root.mkdir())
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
        var capturedDiff: PluginDiffModel? = null
        try {
            val review = runBlocking { owner.review(root) }
            assertEquals(listOf("diff"), review.commands.single().views)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, hash) })
            val target = PreparedSSH("diff-target", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("diff-jump", "127.0.0.1", payload.getString("user"), ports.getInt(0), emptyList())) else emptyList())
            val request = ExtensionRelayBootstrap.Request(payload.getString("gterm"), payload.getString("package"), hash, hash,
                emptyList(), command = "diff", arguments = listOf("--before", JSONObject.quote(before), "--after", JSONObject.quote(after)))
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
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("Owned diff SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull(main { extensions.error })
            await("Diff Session consent") { main { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT } }
            assertEquals(listOf("session.rpc", "ui.diff"), main { extensions.activeSession?.requiredCapabilities })
            assertEquals(PluginDiffModel.methods, main { extensions.activeSession?.methods })
            if (main { extensions.activeSession?.methods } != PluginDiffModel.methods) return
            ui.onNodeWithText("允许并启动").performClick()
            await("Diff Client activation") { main { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE } }
            val diff = main { extensions.diff }; assertNotNull("Native diff contribution must be wired", diff)
            if (diff == null) return
            capturedDiff = diff
            // Drive the test frame clock through the production contribution;
            // polling the worker alone does not compose its LaunchedEffect.
            ui.onNodeWithTag("plugin-diff").assertExists()
            await("First Native page: ${main { extensions.error }}") { main { diff.state.value.page != null || diff.state.value.issue != null } }
            assertNull(main { diff.state.value.issue }); assertEquals(total, main { diff.state.value.descriptor?.total })
            val oldText = StringBuilder(); val newText = StringBuilder(); var consumed = 0
            while (true) {
                val page = main { diff.state.value.page }; assertNotNull(page); if (page == null) return
                assertEquals(consumed, page.offset)
                for (row in page.rows) {
                    val ending = when (row.ending) { PluginDiffRow.Ending.LF -> "\n"; PluginDiffRow.Ending.CRLF -> "\r\n"; PluginDiffRow.Ending.CR -> "\r"; PluginDiffRow.Ending.NONE -> "" }
                    if (row.kind != PluginDiffRow.Kind.INSERT) oldText.append(row.text).append(ending)
                    if (row.kind != PluginDiffRow.Kind.DELETE) newText.append(row.text).append(ending)
                }
                consumed += page.rows.size
                if (!main { diff.canNext }) break
                ui.onNodeWithTag("plugin-diff-next").performScrollTo().performClick()
                val offset = consumed
                await("Next Native diff page") { main { diff.state.value.page?.offset == offset } }
            }
            assertEquals(total, consumed)
            assertArrayEquals(before.toByteArray(Charsets.UTF_8), oldText.toString().toByteArray(Charsets.UTF_8))
            assertArrayEquals(after.toByteArray(Charsets.UTF_8), newText.toString().toByteArray(Charsets.UTF_8))
            if (total > 0) {
                ui.onNodeWithTag("plugin-diff-next").performScrollTo().assertIsNotEnabled()
                while (main { diff.canPrevious }) {
                    val offset = main { diff.state.value.selectedOffset - (diff.state.value.descriptor?.pageSize ?: 3) }
                    ui.onNodeWithTag("plugin-diff-previous").performScrollTo().performClick()
                    await("Previous Native diff page") { main { diff.state.value.page?.offset == offset } }
                }
                ui.onNodeWithTag("plugin-diff-text-0").performScrollTo().assertTextEquals(main { diff.state.value.page!!.rows.first().text })
            } else ui.onNodeWithTag("plugin-diff-empty").performScrollTo().assertTextEquals("两份文本均为空")
            main { extensions.stopRuntime() }
            await("Diff Runtime stop") { main { !extensions.launchBusy && extensions.activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.ACTIVE,
                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.CREATED) } }
            assertFalse(main { diff.state.value.valid }); assertNull(main { diff.state.value.page })
            ui.onNodeWithText("释放插件资源").performScrollTo().assertIsEnabled().performClick()
            await("Diff owner release") { main { extensions.activeSession == null && !extensions.launchBusy } }
            assertNull(main { extensions.error })
        } finally {
            main { capturedDiff?.invalidate(); if (!extensions.launchBusy) extensions.stopRuntime() }
            runBlocking { owner.requestStopRuntime(); owner.close() }
            if (original == null) known.delete() else known.writeBytes(original)
            root.deleteRecursively()
        }
    }
}

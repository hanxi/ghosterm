package com.hanxi.ghosterm

import android.os.Bundle
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
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.DataInputStream
import java.io.File
import java.net.Socket

/** Native Settings and SSH with the Lua Executor and an owned CLI fixture. */
@RunWith(AndroidJUnit4::class)
class ClaudeLuaAppTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun fixture(): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("requires an owned Claude fixture", args.containsKey("fixturePort"))
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000; socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val count = input.readInt(); require(count in 1..1048576)
            return JSONObject(ByteArray(count).also(input::readFully).toString(Charsets.UTF_8))
        }
    }
    private fun control(command: String): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + " $command\n").toByteArray())
            val input = DataInputStream(socket.getInputStream()); val size = input.readInt(); require(size in 1..1048576)
            return JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
    }
    private fun <T> main(block: () -> T): T {
        var result: T? = null; instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(message: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 90000
        while (SystemClock.elapsedRealtime() < end) { if (main(predicate)) return; SystemClock.sleep(20) }
        fail(message)
    }
    @Test fun nativeClaudeConversationOverDirectSSH() = run(false)
    @Test fun nativeClaudeConversationOverProxyJump() = run(true)
    @Test fun emptyArgumentsMatchTheApprovedCandidateSchema() {
        val payload = fixture(); val context = instrumentation.targetContext
        val directory = File(context.cacheDir, "claude-arguments-${System.nanoTime()}"); check(directory.mkdir())
        val files = payload.getJSONObject("packageFiles")
        for (name in files.keys()) {
            check(!name.startsWith('/') && name.split('/').none { it == ".." || it.isEmpty() })
            File(directory, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        val owner = checkNotNull(extensions.controller)
        try {
            val review = runBlocking { owner.review(directory) }
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { owner.approve(review.id, review.digest) })
            repeat(200) { round ->
                val decoded = runBlocking { owner.decodeClientArguments(review.id, review.digest, "ai claude", emptyList()) }
                assertEquals("Empty args must decode consistently at round $round", "{}", decoded)
            }
            var failure: Throwable? = null
            try { runBlocking { owner.decodeClientArguments(review.id, review.digest, "ai claude", listOf("--unknown")) } }
            catch (error: IllegalStateException) { failure = error }
            assertTrue("JNI must preserve the Core argument failure status", failure?.message?.contains("Core 1") == true)
        } finally { runBlocking { owner.close() }; directory.deleteRecursively() }
    }

    private fun run(jump: Boolean) {
        val payload = fixture(); assertEquals("ghosterm-claude-lua", payload.getString("fixture"))
        val context = instrumentation.targetContext
        val directory = File(context.cacheDir, "claude-lua-${java.util.UUID.randomUUID()}"); check(directory.mkdir())
        val files = payload.getJSONObject("packageFiles")
        for (name in files.keys()) {
            check(!name.startsWith('/') && name.split('/').none { it == ".." || it.isEmpty() })
            File(directory, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val known = File(context.filesDir, "home/.ssh/known_hosts"); known.parentFile!!.mkdirs()
        val previousKnown = known.takeIf(File::exists)?.readBytes()
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        val controller = checkNotNull(extensions.controller)
        var conversation: ClaudeConversation? = null
        try {
            val review = runBlocking { controller.review(directory) }
            assertEquals(payload.getString("packageDigest"), review.digest)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { controller.approve(review.id, review.digest) })
            val command = review.commands.single { it.path == "ai claude" }
            val ports = payload.getJSONArray("ports"); val user = payload.getString("user")
            val target = PreparedSSH("claude-target", "127.0.0.1", user, ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("claude-jump", "127.0.0.1", user, ports.getInt(0), emptyList())) else emptyList())
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            val environment = payload.getJSONArray("selectedEnvironment").let { names ->
                (0 until names.length()).map { names.getString(it) }
            }
            val request = ExtensionRelayBootstrap.Request(payload.getString("gtermWrapper"), payload.getString("packageDirectory"),
                review.digest, review.digest, emptyList(), command = "ai claude", workspaceRootDirectory = payload.getString("workspace"),
                processPrograms = listOf(ExtensionRelayBootstrap.ProcessProgram(
                    "claude", payload.getString("program"), "2.1.323", payload.getString("programDigest"))),
                processEnvironmentNames = environment)
            main { extensions.launch(ExtensionPackagesViewModel.Entry(review, "已批准（本进程）"), command, target, request, sessions) }
            val deadline = SystemClock.elapsedRealtime() + 90000
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < deadline) {
                val attempt = main { sessions.connecting }; val prompt = attempt?.auth
                val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || id == null || id == handled) { SystemClock.sleep(20); continue }
                handled = id
                when (prompt.phase) {
                    SSHAuthPhase.HOSTKEY -> main { sessions.decide(checkNotNull(attempt).id, prompt, true) }
                    SSHAuthPhase.CREDENTIALS -> runBlocking { controller.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf()) }
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("owned SSH fixture authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNull("SSH launch failed", main { extensions.error })
            await("Native Session consent missing") { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT }
            ui.onNodeWithText("允许并启动").performClick()
            await("Native Claude adapter missing") { extensions.claudeConversation != null }
            val model = main { checkNotNull(extensions.claudeConversation) }; conversation = model
            ui.onNodeWithTag("plugin-claude-start").performScrollTo().performClick()
            await("Claude readiness missing") { model.state.value.phase == ClaudeConversation.Phase.READY || model.state.value.issue != null }
            if (environment.isNotEmpty() && model.state.value.issue != null) {
                assertTrue("Approved CLI did not receive the scoped environment", control("selected-environment").getBoolean("approved"))
            }
            assertNull(model.state.value.issue)
            main { model.editDraft("explicit CLI fixture 中文😀") }
            ui.onNodeWithTag("plugin-claude-send").performScrollTo().performClick()
            await("Correlated fixture result missing") {
                model.state.value.entries.any { it.title == "本轮已完成" } || model.state.value.issue != null
            }
            if (environment.isNotEmpty()) {
                assertTrue("Approved CLI did not receive the scoped environment", control("selected-environment").getBoolean("approved"))
            }
            assertNull(model.state.value.issue)
            assertNull(model.state.value.activeMessageID)
            assertTrue(model.state.value.entries.any { it.title == "Claude" && it.text == "完整响应😀".repeat(400) })
            main { model.editDraft("interrupt this owned fixture") }
            ui.onNodeWithTag("plugin-claude-send").performScrollTo().performClick()
            val waitingDeadline = SystemClock.elapsedRealtime() + 15000
            while (!control("waiting").getBoolean("ready")) {
                check(SystemClock.elapsedRealtime() < waitingDeadline) { "fixture did not receive the second input" }
                SystemClock.sleep(50)
            }
            await("Second input was not confirmed by the host") {
                model.state.value.phase == ClaudeConversation.Phase.RUNNING && !model.state.value.busy
            }
            ui.onNodeWithTag("plugin-claude-interrupt").performScrollTo().assertIsEnabled().performClick()
            await("Interrupted input did not settle") {
                model.state.value.phase == ClaudeConversation.Phase.READY || model.state.value.issue != null
            }
            assertNull(model.state.value.issue)
            assertNull(model.state.value.activeMessageID)
            val terminal = model.state.value.entries.last()
            assertEquals("本轮未成功", terminal.title)
            assertEquals("Claude CLI interrupted", terminal.text)
            main { model.editDraft("stop this owned fixture") }
            ui.onNodeWithTag("plugin-claude-send").performScrollTo().assertIsEnabled().performClick()
            val stopDeadline = SystemClock.elapsedRealtime() + 15000
            while (!control("waiting-stop").getBoolean("ready")) {
                check(SystemClock.elapsedRealtime() < stopDeadline) { "fixture did not receive the input to stop" }
                SystemClock.sleep(50)
            }
            await("Input to stop was not confirmed by the host") { model.canInterrupt }
            if (payload.optBoolean("disconnect")) {
                val pending = checkNotNull(model.state.value.activeMessageID)
                assertTrue(control("disconnect").getBoolean("requested"))
                await("Disconnected input did not become unknown") {
                    model.state.value.phase in setOf(ClaudeConversation.Phase.UNKNOWN, ClaudeConversation.Phase.LOST)
                }
                assertEquals(pending, model.state.value.activeMessageID)
                assertTrue(model.state.value.issue?.let { "未知" in it || "未确认" in it } == true)
                assertFalse(model.canInterrupt)
                await("Disconnected Runtime did not end") {
                    extensions.activeRuntime?.info?.phase in setOf(ExtensionRuntime.Phase.FAILED, ExtensionRuntime.Phase.STOPPED)
                }
                ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
                await("Plugin owner release missing") { extensions.activeSession == null && extensions.activeRuntime == null && !extensions.launchBusy }
                assertNull(extensions.error)
                return
            }
            ui.onNodeWithTag("plugin-claude-stop").performScrollTo().performClick()
            await("Claude stop/reap missing") { model.state.value.phase == ClaudeConversation.Phase.CLOSED || model.state.value.issue != null }
            assertEquals(ClaudeConversation.Phase.CLOSED, model.state.value.phase)
            assertTrue(model.state.value.issue?.contains("结果尚未确认") == true)
            ui.onNodeWithText("停止插件").performScrollTo().performClick()
            await("Runtime stop missing") { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.STOPPED }
            assertSame("Ended conversation remains readable until explicit release", model, main { extensions.claudeConversation })
            ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
            await("Plugin owner release missing") { extensions.activeSession == null && extensions.activeRuntime == null && !extensions.launchBusy }
            assertNull(extensions.error)
        } catch (failure: Throwable) {
            instrumentation.sendStatus(0, Bundle().apply { putString("fixtureState", "Claude fixture: issue=${conversation?.state?.value?.issue}; " +
                "phase=${conversation?.state?.value?.phase}; owner=${main { extensions.error }}; runtime=${main { extensions.activeRuntime?.info }}") })
            throw failure
        } finally {
            runBlocking { controller.close() }
            if (previousKnown == null) known.delete() else known.writeBytes(previousKnown)
            directory.deleteRecursively()
        }
    }
}

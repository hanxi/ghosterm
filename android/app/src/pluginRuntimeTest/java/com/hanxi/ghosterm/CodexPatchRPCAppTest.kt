package com.hanxi.ghosterm

import android.os.SystemClock
import android.os.Bundle
import android.util.Base64
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.espresso.Espresso
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

/** Production Settings/RPC/SSH; the explicitly mapped app-server fixture never calls a model. */
@RunWith(AndroidJUnit4::class)
class CodexPatchRPCAppTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()

    private fun fixture(action: String = "fixture"): JSONObject {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", checkNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((checkNotNull(args.getString("fixtureToken")) + "\n" + action + "\n").toByteArray())
            val input = DataInputStream(socket.getInputStream())
            val size = input.readInt()
            check(size in 1..1_048_576)
            return JSONObject(ByteArray(size).also(input::readFully).toString(Charsets.UTF_8))
        }
    }

    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST")
        return result as T
    }

    private fun await(label: String, seconds: Int = 90, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + seconds * 1000
        while (SystemClock.elapsedRealtime() < end) {
            if (main(predicate)) return
            SystemClock.sleep(20)
        }
        fail(label)
    }

    private fun diff(revision: Int) = "@@ -1 +1 @@\n" +
        (0 until 600).joinToString("") { "+REV-$revision row-$it 中文😀\n" } + "+FINAL-REV-$revision\n"

    @Test fun completePatchAndVersionedApprovalOverDirectSSH() = run(false)
    @Test fun completePatchAndVersionedApprovalOverProxyJump() = run(true)

    private fun run(jump: Boolean) {
        assertTrue("requires explicit JS-enabled instrumentation", BuildConfig.PLUGIN_JS_ENABLED)
        val payload = fixture()
        assertEquals("ghosterm-patch-rpc", payload.getString("fixture"))
        val context = instrumentation.targetContext
        val packageDir = File(context.cacheDir, "codex-patch-${java.util.UUID.randomUUID()}")
        check(packageDir.mkdir())
        val files = payload.getJSONObject("packageFiles")
        for (name in files.keys()) {
            check(!name.startsWith('/') && name.split('/').none { it == ".." || it.isEmpty() })
            File(packageDir, name).apply { parentFile!!.mkdirs(); writeBytes(Base64.decode(files.getString(name), Base64.DEFAULT)) }
        }
        val digest = MessageDigest.getInstance("SHA-256").digest(File(packageDir, "integrity.json").readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 255) }
        assertEquals(payload.getString("packageDigest"), digest)
        val known = File(context.filesDir, "home/.ssh/known_hosts")
        check(known.parentFile!!.isDirectory || known.parentFile!!.mkdirs())
        val originalKnown = known.takeIf(File::exists)?.readBytes()
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        val sessions = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        val controller = checkNotNull(extensions.controller)
        try {
            val review = runBlocking { controller.review(packageDir) }
            assertEquals(digest, review.digest)
            assertEquals(ExtensionRegistry.Status.OK, runBlocking { controller.approve(review.id, review.digest) })
            val entry = ExtensionPackagesViewModel.Entry(review, "已批准（本进程）")
            val ports = payload.getJSONArray("ports")
            val user = payload.getString("user")
            val target = PreparedSSH("patch-target", "127.0.0.1", user, ports.getInt(1), emptyList(),
                if (jump) listOf(PreparedSSH("patch-jump", "127.0.0.1", user, ports.getInt(0), emptyList())) else emptyList())
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            val request = ExtensionRelayBootstrap.Request(
                executable = payload.getString("gtermWrapper"), executorDirectory = payload.getString("packageDirectory"),
                executorDigest = digest, clientDigest = digest, methods = emptyList(), command = "ai codex",
                workspaceRootDirectory = payload.getString("workspace"), processPrograms = listOf(
                    ExtensionRelayBootstrap.ProcessProgram("codex", payload.getString("program"),
                        "codex-cli 0.166.0", payload.getString("programDigest"))))
            main { extensions.launch(entry, review.commands.single { it.path == "ai codex" }, target, request, sessions) }
            val end = SystemClock.elapsedRealtime() + 90_000
            var handled: Triple<Long, Int, SSHAuthPhase>? = null
            while (main { extensions.activeSession == null && extensions.error == null } && SystemClock.elapsedRealtime() < end) {
                val attempt = main { sessions.connecting }
                val prompt = attempt?.auth
                val id = prompt?.let { Triple(it.request, it.hop, it.phase) }
                if (prompt == null || id == null || id == handled) { SystemClock.sleep(20); continue }
                handled = id
                when (prompt.phase) {
                    SSHAuthPhase.HOSTKEY -> main { sessions.decide(checkNotNull(attempt).id, prompt, true) }
                    SSHAuthPhase.CREDENTIALS -> runBlocking {
                        controller.submitSSHPrivateKey(prompt, payload.getString("pem").toByteArray(), byteArrayOf())
                    }
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail("owned fixture SSH authentication failed")
                    else -> SystemClock.sleep(20)
                }
            }
            assertNotNull("SSH Session missing: ${main { extensions.error }}", main { extensions.activeSession })
            await("Session did not reach consent") { extensions.activeSession?.info?.phase == ExtensionSession.Phase.CONSENT }
            ui.onNodeWithText("允许并启动").performClick()
            await("Client Runtime did not become available for interaction") {
                extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.ACTIVE &&
                    extensions.canInteractWithRuntime && !extensions.codex.callBusy
            }
            ui.onNodeWithTag("plugin-codex-start-thread").performScrollTo().assertIsEnabled().performClick()
            await("thread did not arrive: ${main { extensions.error }}") { extensions.codex.threadID == "thread-fixture" && !extensions.codex.callBusy }
            ui.onNodeWithTag("plugin-codex-message").performScrollTo().performTextInput("exercise isolated protocol fixture")
            Espresso.closeSoftKeyboard()
            ui.onNodeWithTag("plugin-codex-send").performScrollTo().performClick()
            await("patch/approval did not arrive") { extensions.codex.approvals.size == 1 && !extensions.codex.callBusy || extensions.error != null }
            assertNull(extensions.error)
            val approval = main { extensions.codex.approvals.single() }
            val old = main { extensions.codex.timeline.single { it.changeReference != null }.changeReference!! }
            assertEquals("s:file:审批", approval.id)
            assertTrue(main { extensions.codex.timeline.single { it.changeReference != null }.fileChanges.isEmpty() })
            assertEquals(10, main { extensions.codex.timeline.single { it.changeReference != null }.omittedChangeCount })
            ui.onNodeWithTag("plugin-codex-approval-allow").performScrollTo().assertIsNotEnabled()
            fun read(reference: CodexChangeReference, revision: Int) {
                assertTrue("Read must be admitted by the current Runtime", main { extensions.canReadCodexChanges })
                ui.onAllNodesWithTag("plugin-codex-read-change-${reference.snapshotID}")[0]
                    .performScrollTo().assertIsEnabled().performClick()
                await("Native read click did not start revision $revision", 5) { extensions.changeReview.state.value != null }
                await("complete revision $revision was not published", 120) {
                    extensions.changeReview.state.value?.let { it.document != null || it.issue != null } == true || extensions.error != null
                }
                assertNull(extensions.error)
                assertNull("Complete patch read failed", main { extensions.changeReview.state.value?.issue })
                val document = main { checkNotNull(extensions.changeReview.state.value?.document) }
                assertEquals(10, document.files.size)
                assertEquals("nested/" + "x".repeat(4096) + "/source.swift", document.files[0].path)
                assertArrayEquals(diff(revision).toByteArray(), document.files[0].diff.toByteArray())
                val text = ui.onNodeWithTag("plugin-codex-full-diff-text")
                text.assertTextEquals(diff(revision))
                text.assert(SemanticsMatcher.keyNotDefined(SemanticsActions.SetText))
                text.performSemanticsAction(SemanticsActions.SetSelection) { select -> assertTrue(select(0, 8, false)) }
                assertEquals(reference.snapshotID, main { extensions.codexApprovalSnapshot(approval) })
            }
            read(old, 1)
            assertTrue(fixture("update").getBoolean("ok"))
            await("new patch did not invalidate old review") {
                extensions.codex.timeline.first { it.changeReference != null }.changeReference?.snapshotID != old.snapshotID &&
                    extensions.changeReview.state.value == null
            }
            ui.onNodeWithTag("plugin-codex-approval-allow").performScrollTo().assertIsNotEnabled()
            val input = JSONObject().put("requestID", approval.id).put("decisionJSON", "\"accept\"")
                .put("snapshotID", old.snapshotID).toString().toByteArray()
            val admission = runBlocking { controller.requestRuntimeExecutor("codex.approval.respond", input) }
            assertEquals(ExtensionSession.RPCStatus.OK, admission.status)
            val handle = checkNotNull(admission.handle)
            var result: ExtensionSession.RPCResult? = null
            val rpcEnd = SystemClock.elapsedRealtime() + 15_000
            while (SystemClock.elapsedRealtime() < rpcEnd) {
                val (status, value) = runBlocking { controller.runtimeExecutorRequestResult(handle) }
                assertEquals(ExtensionSession.RPCStatus.OK, status)
                if (value?.confirmed == true) { result = value; break }
                SystemClock.sleep(20)
            }
            assertNotNull("stale-version RPC did not confirm", result)
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, result!!.state)
            val refusal = JSONObject(result.outcome.toString(Charsets.UTF_8))
            assertEquals("success", refusal.getString("kind"))
            assertFalse(refusal.getJSONObject("value").getBoolean("accepted"))
            assertEquals(ExtensionSession.RPCStatus.OK, runBlocking { controller.forgetRuntimeExecutorRequest(handle) })
            assertFalse(fixture("audit").has("request"))
            val current = main { extensions.codex.timeline.single { it.changeReference != null }.changeReference!! }
            read(current, 2)
            ui.onNodeWithTag("plugin-codex-approval-allow").performScrollTo().assertIsEnabled().performClick()
            await("approval did not complete") { extensions.codex.approvals.isEmpty() && !extensions.codex.turnActive && !extensions.codex.callBusy }
            val audit = fixture("audit")
            assertEquals(2, audit.getInt("revision"))
            assertEquals("file:审批", audit.getJSONObject("request").getString("id"))
            assertEquals("accept", audit.getJSONObject("request").getJSONObject("result").getString("decision"))
            assertFalse(audit.getJSONObject("request").has("snapshotID"))
            ui.onNodeWithText("停止插件").performScrollTo().performClick()
            await("stop did not finish") { extensions.activeRuntime?.info?.phase == ExtensionRuntime.Phase.STOPPED }
            ui.onNodeWithText("释放插件资源").performScrollTo().performClick()
            await("release did not clear Session/review") {
                extensions.activeRuntime == null && extensions.activeSession == null &&
                    extensions.changeReview.state.value == null && !extensions.launchBusy
            }
            assertNull(extensions.error)
        } catch (failure: Throwable) {
            val diagnostics = "Codex patch fixture failed: error=${main { extensions.error }} " +
                "runtime=${main { extensions.activeRuntime?.info }} session=${main { extensions.activeSession?.info }} " +
                "thread=${main { extensions.codex.threadID }} approvals=${main { extensions.codex.approvals.size }} " +
                "review=${main { extensions.changeReview.state.value?.let { "${it.reference.snapshotID}/${it.receivedBytes}/${it.issue}" } }}"
            instrumentation.sendStatus(0, Bundle().apply { putString("fixtureState", diagnostics) })
            throw failure
        } finally {
            runBlocking { controller.close() }
            if (originalKnown == null) known.delete() else known.writeBytes(originalKnown)
            packageDir.deleteRecursively()
        }
    }
}

package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import android.os.SystemClock
import com.hanxi.ghosterm.bridge.NativeTerminalSession
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.util.UUID

/** Focused parser / production form smoke. The route callback is injected;
 * actual SSH ownership and device matrices remain separate verification. */
@RunWith(AndroidJUnit4::class)
class PluginForegroundInspectionTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val instance = "1".repeat(32)
    private fun declaration(): ByteArray = JSONObject().apply {
        put("schemaVersion", 1); put("instance", instance); put("runtime", "portable-js")
        put("executorDigest", "b".repeat(64)); put("clientDigest", "a".repeat(64)); put("command", "fixture")
        put("arguments", JSONArray(listOf("./文档.md", ""))); put("methods", JSONArray(listOf("fixture.echo")))
        put("hostMethods", JSONArray()); put("clientCapabilities", JSONArray()); put("authorizationGranted", false)
    }.toString().toByteArray()

    @Test fun declarationPreservesLiteralArgumentsAndRejectsAmbiguityOrGrants() {
        val bytes = declaration()
        val value = PluginForegroundDeclaration.parse(bytes, instance)
        assertEquals(listOf("./文档.md", ""), value.arguments)
        assertEquals("b".repeat(64), value.executorDigest)
        val source = bytes.toString(Charsets.UTF_8)
        for (bad in listOf(
            source.replace("\"authorizationGranted\":false", "\"authorizationGranted\":true").toByteArray(),
            source.replace("\"schemaVersion\":1", "\"schemaVersion\":1,\"schemaVersion\":1").toByteArray(),
            source.dropLast(1).plus(",\"directory\":\"/redirect\"}").toByteArray(),
            bytes + byteArrayOf(0xc0.toByte(), 0x80.toByte()),
            ByteArray(PluginForegroundDeclaration.MAXIMUM_BYTES + 1),
        )) assertTrue(runCatching { PluginForegroundDeclaration.parse(bad, instance) }.isFailure)
        assertTrue(runCatching { PluginForegroundDeclaration.parse(bytes, "2".repeat(32)) }.isFailure)
        assertEquals("exec 'gterm' 'relay' '--inspect' '/work/front' '--instance' '$instance'",
            PluginForegroundDeclaration.bootstrap("gterm", "/work/front", instance))
        assertTrue(runCatching { PluginForegroundDeclaration.bootstrap("gterm", "/work/../front", instance) }.isFailure)
    }

    @Test fun inspectionAdoptsExactDeclarationClearsApprovalsAndInvalidatesOnRetarget() {
        val command = ExtensionController.Command("fixture", "Fixture", "schemas/input.json", emptyList())
        val review = ExtensionController.Review("org.ghosterm.fixture", "0.1.0", "fixture", "MIT", "a".repeat(64),
            "portable-js", listOf("session.rpc", "resource.receive"), emptyList(), false, false, listOf(command))
        var captured: ExtensionRelayBootstrap.Request? = null
        var inspections = 0
        ui.setContent { MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
            listOf("host"), onDismiss = {}, onLaunch = { _, request, _ -> captured = request },
            onInspect = { alias, executable, directory, selected ->
                assertEquals("host", alias); assertEquals("gterm", executable)
                assertEquals("/private/front", directory); assertEquals(instance, selected)
                inspections++; PluginForegroundDeclaration.parse(declaration(), selected)
            }) } }
        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-directory").performTextInput("/private/front")
        ui.onNodeWithTag("plugin-ipc-instance").performTextInput(instance)
        Espresso.closeSoftKeyboard()
        for (tag in listOf("plugin-ipc-files", "plugin-ipc-processes", "plugin-ipc-allow")) {
            ui.onNodeWithTag(tag).performScrollTo().performClick()
        }
        ui.onNodeWithTag("plugin-ipc-inspect").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-adopt").performScrollTo().performClick()
        for (tag in listOf("plugin-ipc-files", "plugin-ipc-processes", "plugin-ipc-allow")) ui.onNodeWithTag(tag).assertIsOff()
        ui.runOnIdle { assertNull(captured); assertEquals(1, inspections) }
        ui.onNodeWithTag("plugin-ipc-files").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle {
            assertEquals("b".repeat(64), captured?.executorDigest)
            assertEquals(listOf("./文档.md", ""), captured?.arguments)
            assertEquals(listOf("fixture.echo"), captured?.methods)
            assertTrue(captured?.attachment?.authorized == true && captured?.attachment?.scopedFiles == true)
            assertFalse(captured?.attachment?.remoteProcesses ?: true)
        }
        ui.onNodeWithTag("plugin-ipc-instance").performScrollTo().performTextReplacement("2".repeat(32))
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-ipc-declaration").assertDoesNotExist()
        ui.onNodeWithTag("plugin-ipc-allow").assertIsOff()
        ui.onNodeWithTag("plugin-ipc-files").assertIsOff()
    }

    @Test fun terminalCandidateRequiresExplicitHostSelectionBeforeInspection() {
        val command = ExtensionController.Command("fixture", "Fixture", "schemas/input.json", emptyList())
        val digest = "a".repeat(64)
        val review = ExtensionController.Review("org.ghosterm.fixture", "0.1.0", "fixture", "MIT", digest,
            "portable-js", emptyList(), emptyList(), false, false, listOf(command))
        val candidate = FrontendCandidate(UUID.randomUUID(), UUID.randomUUID(), "终端来源",
            NativeTerminalSession.DiscoveryOffer("/private/front", instance, digest, 1uL), SystemClock.elapsedRealtime())
        var inspectedHost: String? = null
        ui.setContent { MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
            listOf("default-host", "chosen-host"), onDismiss = {}, onLaunch = { _, _, _ -> fail("检查不启动插件") },
            frontendCandidates = listOf(candidate), onInspect = { alias, _, directory, selected ->
                inspectedHost = alias
                assertEquals(candidate.offer.directory, directory)
                PluginForegroundDeclaration.parse(declaration(), selected)
            }) } }

        ui.onNodeWithTag("plugin-discovery-use-${candidate.id}").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-inspect").assertIsNotEnabled()
        ui.onNodeWithText("连接并请求授权").assertIsNotEnabled()
        ui.onNodeWithText("选择 SSH 主机").performScrollTo().performClick()
        ui.onNodeWithText("chosen-host").performClick()
        ui.onNodeWithTag("plugin-ipc-inspect").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-declaration").assertExists()
        ui.runOnIdle { assertEquals("chosen-host", inspectedHost) }
    }
}

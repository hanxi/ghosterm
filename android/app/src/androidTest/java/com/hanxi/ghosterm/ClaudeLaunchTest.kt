package com.hanxi.ghosterm

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.espresso.Espresso
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.json.JSONArray
import org.json.JSONObject

@RunWith(AndroidJUnit4::class)
class ClaudeLaunchTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun request() = ExtensionRelayBootstrap.Request("gterm", "/k", "a".repeat(64), "b".repeat(64),
        emptyList(), command = "ai claude", workspaceRootDirectory = "/w", processPrograms = listOf(
            ExtensionRelayBootstrap.ProcessProgram("claude", "/bin/claude", "2.1.323", "c".repeat(64),
                listOf("/e 中😀'quoted.mjs", "--profile", "/p"))), processEnvironmentNames = listOf("CLAUDE_CONFIG_DIR"))
    private fun rejected(message: String, block: () -> Unit) {
        var failed = false
        try { block() } catch (_: IllegalArgumentException) { failed = true }
        assertTrue(message, failed)
    }

    @Test fun fixedArgumentsAndRemoteEnvironmentNamesAreQuotedInOrder() {
        val command = ExtensionRelayBootstrap.build(request())
        assertTrue("Fixed entry and profile argv must be emitted", command.contains(
            "'--process-argument' 'claude' '/e 中😀'\\''quoted.mjs' '--process-argument' 'claude' '--profile' '--process-argument' 'claude' '/p'"))
        assertTrue("Selected environment names must be emitted", command.contains("'--process-environment' 'CLAUDE_CONFIG_DIR'"))
        assertTrue(command.endsWith("'--' '/k' 'ai claude'"))
        val mapping = request().processPrograms.single()
        val source = "[{\"name\":\"claude\",\"executable\":\"/bin/claude\",\"version\":\"2.1.323\",\"sha256\":\"${mapping.sha256}\",\"prefixArguments\":[\"/e 中😀'quoted.mjs\",\"--profile\",\"/p\"]}]"
        assertEquals(mapping, ExtensionRelayBootstrap.parseProcessProgramsJson(source).single())
        assertEquals(listOf("CLAUDE_CONFIG_DIR"), ExtensionRelayBootstrap.parseProcessEnvironmentJson("[\"CLAUDE_CONFIG_DIR\"]"))
    }

    @Test fun invalidPrefixArgumentsCannotReachTheRemoteCommand() {
        val request = request(); val program = request.processPrograms.single()
        for (args in listOf(List(17) { "x" }, listOf("x".repeat(4097)), List(4) { "x".repeat(4096) },
                listOf("bad\nentry"), listOf("bad\rentry"), listOf("bad\u0000entry"), listOf("\uD800"))) {
            val mapping = JSONObject().put("name", "n").put("executable", "/n").put("version", "1")
                .put("sha256", "a".repeat(64)).put("prefixArguments", JSONArray(args))
            rejected("Invalid fixed arguments must be rejected in JSON") {
                ExtensionRelayBootstrap.parseProcessProgramsJson(JSONArray().put(mapping).toString())
            }
            rejected("Invalid fixed arguments must fail before SSH") {
                ExtensionRelayBootstrap.build(request.copy(processPrograms = listOf(program.copy(
                    name = "n", executable = "/n", version = "1", prefixArguments = args))))
            }
        }
        for (suffix in listOf(",\"prefixArguments\":null", ",\"prefixArguments\":[1]", ",\"prefixArguments\":[],\"\\u0070refixArguments\":[]")) {
            rejected("Fixed argv JSON must be typed and duplicate-free") {
                ExtensionRelayBootstrap.parseProcessProgramsJson("[{\"name\":\"n\",\"executable\":\"/n\",\"version\":\"1\",\"sha256\":\"${"a".repeat(64)}\"$suffix}]")
            }
        }
    }

    @Test fun environmentNamesRequireMappingAndNeverAcceptValuesOrDefaults() {
        for (names in listOf(listOf("HOME"), listOf("PATH"), listOf("TMPDIR"), listOf("CODEX_HOME"), listOf("XDG_CONFIG_HOME"),
                listOf("API_KEY=value"), listOf("9BAD"), listOf("中文"), listOf("A", "A"), listOf("A".repeat(129)), List(28) { "V$it" })) {
            rejected("Invalid environment selections must fail") { ExtensionRelayBootstrap.build(request().copy(processEnvironmentNames = names)) }
        }
        rejected("Environment selection requires a mapping") { ExtensionRelayBootstrap.build(request().copy(processPrograms = emptyList())) }
        rejected("Mapping requires explicit workspace") { ExtensionRelayBootstrap.build(request().copy(workspaceRootDirectory = null)) }
        rejected("SSH command length remains bounded") {
            ExtensionRelayBootstrap.build(request().copy(arguments = listOf("x".repeat(1024))))
        }
    }

    @Test fun productionLaunchFormKeepsFixedArgumentsAndEnvironmentSelection() {
        val command = ExtensionController.Command("ai claude", "Claude", "schemas/command.json", emptyList())
        val review = ExtensionController.Review(id = "org.ghosterm.ai-claude", version = "0.1.0", publisher = "fixture", license = "MIT",
            digest = "b".repeat(64), runtime = "lua", requiredCapabilities = listOf("session.rpc"),
            optionalMissingCapabilities = emptyList(), authorizationGranted = false, publisherAuthenticated = false,
            commands = listOf(command))
        var launched: ExtensionRelayBootstrap.Request? = null
        var attempts = 0
        val preflightError = "SSH 配置已变化，请重新选择主机"
        ui.setContent { MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
            listOf("fixture"), {}, { _, request, _ ->
                if (++attempts == 1) error(preflightError)
                launched = request
            }) } }
        ui.onNodeWithTag("plugin-executor-directory").performScrollTo().performTextInput("/k")
        ui.onNodeWithTag("plugin-workspace-root").performScrollTo().performTextInput("/w")
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextInput("a".repeat(64))
        ui.onNodeWithTag("plugin-process-programs").performScrollTo().performTextReplacement(
            "[{\"name\":\"claude\",\"executable\":\"/bin/claude\",\"version\":\"2.1.323\",\"sha256\":\"${"c".repeat(64)}\",\"prefixArguments\":[\"/e\",\"--profile\",\"/p\"]}]")
        ui.onNodeWithTag("plugin-process-environment").performScrollTo().performTextReplacement("[\"CLAUDE_CONFIG_DIR\"]")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").assertIsEnabled().performClick()
        ui.onNodeWithText(preflightError).assertIsDisplayed()
        ui.runOnIdle { assertNull(launched) }
        ui.onNodeWithText("连接并请求授权").assertIsEnabled().performClick()
        ui.waitForIdle()
        if (launched == null) fail("Launch form did not submit:\n" + ui.onAllNodes(isRoot(), useUnmergedTree = true).printToString(maxDepth = 15))
        ui.runOnIdle {
            assertEquals("Native form must preserve the chosen fixed argv", listOf("/e", "--profile", "/p"), launched?.processPrograms?.single()?.prefixArguments)
            assertEquals("Native form must pass selected names", listOf("CLAUDE_CONFIG_DIR"), launched?.processEnvironmentNames)
        }
    }

    @Test fun inactiveViewModelCannotDispatchClaudeActions(): Unit = runBlocking {
        withContext(Dispatchers.Main) {
            val application = InstrumentationRegistry.getInstrumentation().targetContext.applicationContext as Application
            val model = ExtensionPackagesViewModel(application)
            var failed = false
            try { model.requestClaudeAction("start", "{}".toByteArray()) } catch (_: IllegalStateException) { failed = true }
            assertTrue("Claude action requires this active Runtime and complete methods", failed)
            model.controller?.close()
            Unit
        }
    }
}

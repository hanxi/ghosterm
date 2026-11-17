package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.espresso.Espresso
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ExecutorExecutionTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val digest = "a".repeat(64)
    private fun node(executable: String = "/n", version: String = "26.4.0", sha256: String = digest,
        sdk: String = "/s", catalog: String = "b".repeat(64)) = ExtensionRelayBootstrap.Execution.Runtime.Node(
        ExtensionRelayBootstrap.NodeDependency(executable, version, sha256, sdk, catalog))
    private fun request(runtime: ExtensionRelayBootstrap.Execution.Runtime? = null, temporary: String = "/t",
        authorized: Boolean = true, environment: List<String> = emptyList()) = ExtensionRelayBootstrap.Request(
        "gterm", "/k", digest, digest, emptyList(), command = "fixture", processEnvironmentNames = environment,
        execution = runtime?.let { ExtensionRelayBootstrap.Execution(it, temporary, authorized) })
    private fun rejected(block: () -> Unit) {
        var failed = false
        try { block() } catch (_: IllegalArgumentException) { failed = true }
        assertTrue("Invalid execution configuration must fail before SSH", failed)
    }
    @Test fun productionSignedReleaseFormRequiresFreshPolicyApprovalAndPreservesSelection() {
        var launched: ExtensionRelayBootstrap.Request? = null
        form { launched = it }
        fillCommon()
        ui.onNodeWithTag("plugin-executor-release-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-executor-release-directory").performScrollTo().performTextInput("/installed/release")
        ui.onNodeWithTag("plugin-executor-release-policy").performScrollTo().performTextInput("/selected/policy.json")
        ui.onNodeWithTag("plugin-executor-release-policy-sha256").performScrollTo().performTextInput(digest)
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertNull(launched) }
        ui.onNodeWithTag("plugin-executor-release-allow").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertEquals(ExtensionRelayBootstrap.ReleaseEvidence("/installed/release", "/selected/policy.json", digest, true), launched?.releaseEvidence) }
        ui.onNodeWithTag("plugin-executor-release-policy").performScrollTo().performTextReplacement("/changed/policy.json")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-executor-release-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("/selected/policy.json", launched?.releaseEvidence?.policyPath) }
        ui.onNodeWithTag("plugin-executor-release-allow").performClick()
        ui.onNodeWithTag("plugin-executor-variant").performScrollTo().performTextReplacement("portable-2")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-executor-release-allow").performScrollTo().assertIsOff()
        ui.onNodeWithTag("plugin-executor-release-allow").performClick()
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextReplacement("b".repeat(64))
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-executor-release-allow").performScrollTo().assertIsOff()
    }
    @Test fun productionVariantSelectionPreservesIDAndRevokesExecutionApprovalOnEdits() {
        var launched: ExtensionRelayBootstrap.Request? = null
        form { launched = it }; fillCommon(); choose("Exec")
        ui.onNodeWithTag("plugin-execution-tmpdir").performScrollTo().performTextInput("/stage")
        ui.onNodeWithTag("plugin-executor-variant").performScrollTo().performTextInput("native-2")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertEquals("native-2", launched?.variantID); assertEquals(true, launched?.execution?.authorized) }
        ui.onNodeWithTag("plugin-executor-variant").performScrollTo().performTextReplacement("native-3")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("native-2", launched?.variantID) }
    }
    @Test fun portableDefaultHasNoProcessExecutionFlags() {
        assertNull(request().execution)
        assertEquals("exec 'gterm' 'relay' '--stdio' '--trust-development-digest' '$digest' '--client-digest' '$digest' '--' '/k' 'fixture'",
            ExtensionRelayBootstrap.build(request()))
    }
    @Test fun foregroundAttachmentBindsExplicitInstanceAndCannotReconfigureExecutor() {
        val attachment=ExtensionRelayBootstrap.Attachment("/private/front","b".repeat(32),true)
        val request=request().copy(executorDirectory="",attachment=attachment,arguments=listOf("", "中'😀"))
        assertEquals("exec 'gterm' 'relay' '--attach' '/private/front' '--instance' '${"b".repeat(32)}' '--' 'fixture' '' '中'\\''😀'",ExtensionRelayBootstrap.build(request))
        for(path in listOf("relative","/private/front/","/private//front","/private/./front","/private/../front","/private/\nfront"))
            rejected { ExtensionRelayBootstrap.build(request.copy(attachment=attachment.copy(directory=path))) }
        for(id in listOf("","b".repeat(31),"B".repeat(32)))rejected { ExtensionRelayBootstrap.build(request.copy(attachment=attachment.copy(instance=id))) }
        rejected { ExtensionRelayBootstrap.build(request.copy(attachment=attachment.copy(authorized=false))) }
        rejected { ExtensionRelayBootstrap.build(request.copy(executorDirectory="/other")) }
        rejected { ExtensionRelayBootstrap.build(request.copy(workspaceRootDirectory="/other")) }
        rejected { ExtensionRelayBootstrap.build(request.copy(execution=ExtensionRelayBootstrap.Execution(ExtensionRelayBootstrap.Execution.Runtime.Exec,"/t",true))) }
    }
    @Test fun productionAttachmentFormCapturesInstanceAndRevokesApprovalOnEdits() {
        var launched:ExtensionRelayBootstrap.Request?=null
        form { launched=it };fillCommon()
        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-executor-directory").assertDoesNotExist()
        ui.onNodeWithTag("plugin-executor-runtime").assertDoesNotExist()
        ui.onNodeWithTag("plugin-ipc-directory").performScrollTo().performTextInput("/private/front")
        ui.onNodeWithTag("plugin-ipc-instance").performScrollTo().performTextInput("b".repeat(32))
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").performClick();ui.runOnIdle { assertNull(launched) }
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertEquals("b".repeat(32),launched?.attachment?.instance);assertEquals(true,launched?.attachment?.authorized);assertEquals("",launched?.executorDirectory);assertNull(launched?.execution) }
        ui.onNodeWithTag("plugin-ipc-instance").performScrollTo().performTextReplacement("c".repeat(32))
        Espresso.closeSoftKeyboard();ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("b".repeat(32),launched?.attachment?.instance) }
        ui.onNodeWithTag("plugin-ipc-allow").performClick().assertIsOn()
        ui.onNodeWithTag("plugin-ipc-processes").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff()
    }
    @Test fun nodeAndExecRequireSeparateApprovalAndNeverGrantBusinessProcesses() {
        for (runtime in listOf(node(), ExtensionRelayBootstrap.Execution.Runtime.Exec)) {
            rejected { ExtensionRelayBootstrap.build(request(runtime, authorized = false)) }
            val command = ExtensionRelayBootstrap.build(request(runtime, environment = listOf("FIXTURE_ROOT")))
            assertTrue(command.startsWith("TMPDIR='/t' exec "))
            assertTrue(command.contains("'--allow-process-execution'"))
            assertTrue(command.contains("'--process-environment' 'FIXTURE_ROOT'"))
            assertFalse(command.contains("--process-program")); assertFalse(command.contains("--root"))
        }
        val command = ExtensionRelayBootstrap.build(request(node()))
        assertTrue(command.contains("'--runtime' 'node' '--allow-process-execution' '--node-runtime' '/n' '26.4.0' '$digest' '--node-sdk' '/s' '${"b".repeat(64)}'"))
        val exec = ExtensionRelayBootstrap.build(request(ExtensionRelayBootstrap.Execution.Runtime.Exec))
        assertTrue(exec.contains("'--runtime' 'exec'")); assertFalse(exec.contains("--node-sdk"))
        rejected { ExtensionRelayBootstrap.build(request(environment = listOf("FIXTURE_ROOT"))) }
    }
    @Test fun invalidDependenciesAndTemporaryDirectoriesAreRejected() {
        for (runtime in listOf(node(executable = "node"), node(executable = "/n\nentry"), node(version = "v26.4.0"),
            node(version = ""), node(version = "26 4"), node(version = "1".repeat(129)), node(sha256 = "a"),
            node(sha256 = "A".repeat(64)), node(sdk = "sdk"), node(sdk = "/s\u0000bad"),
            node(catalog = ""), node(catalog = "g".repeat(64)), node(executable = "/\uD800"))) {
            rejected { ExtensionRelayBootstrap.build(request(runtime)) }
        }
        for (tmp in listOf("", "relative", "/t\nentry", "/t\rentry", "/t\u0000entry", "/" + "x".repeat(4096))) {
            rejected { ExtensionRelayBootstrap.build(request(ExtensionRelayBootstrap.Execution.Runtime.Exec, temporary = tmp)) }
        }
        rejected { ExtensionRelayBootstrap.build(request(node(executable = "/" + "x".repeat(1000)))) }
    }
    @Test fun quotedDependenciesAndTMPDIRRemainLiteral() {
        val malicious = "/中'$(touch SHOULD_NOT_EXIST);😀"
        val command = ExtensionRelayBootstrap.build(request(node(executable = malicious, sdk = malicious), temporary = malicious))
        assertTrue(command.startsWith("TMPDIR='/中'\\''$(touch SHOULD_NOT_EXIST);😀' exec "))
        assertTrue(command.contains("'--node-runtime' '/中'\\''$(touch SHOULD_NOT_EXIST);😀'"))
        assertTrue(command.contains("'--node-sdk' '/中'\\''$(touch SHOULD_NOT_EXIST);😀'"))
    }
    @Test fun productionModeSwitchRevokesFreshAndAttachmentApproval() {
        form { };fillCommon();choose("Exec")
        ui.onNodeWithTag("plugin-execution-tmpdir").performScrollTo().performTextInput("/t")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().performClick().assertIsOn()
        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-mode").performClick()
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().performClick().assertIsOn()
        ui.onNodeWithTag("plugin-ipc-mode").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-mode").performClick()
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff()
    }
    @Test fun productionFormRequiresApprovalAndCapturesExecWithoutNodeDependencies() {
        var launched: ExtensionRelayBootstrap.Request? = null
        form { launched = it }
        fillCommon()
        choose("Exec")
        ui.onNodeWithTag("plugin-execution-tmpdir").performScrollTo().performTextInput("/t")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertNull(launched) }
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle {
            assertNotNull("Production form must retain execution configuration", launched?.execution)
            val execution = checkNotNull(launched?.execution)
            assertEquals(ExtensionRelayBootstrap.Execution.Runtime.Exec, execution.runtime)
            assertEquals("/t", execution.temporaryDirectory); assertTrue(execution.authorized)
            assertTrue(ExtensionRelayBootstrap.build(checkNotNull(launched)).contains("'--runtime' 'exec'"))
        }
    }
    @Test fun productionFormRevokesApprovalOnDependencyEditsAndRuntimeSwitch() {
        var launched: ExtensionRelayBootstrap.Request? = null
        val commandPath = mutableStateOf("fixture")
        form(commandPath) { launched = it }
        fillCommon()
        choose("Node")
        val fields = listOf("plugin-execution-tmpdir" to "/t", "plugin-node-executable" to "/n",
            "plugin-node-version" to "26.4.0", "plugin-node-sha256" to digest,
            "plugin-node-sdk-directory" to "/s", "plugin-node-sdk-sha256" to "b".repeat(64))
        for ((field, value) in fields) {
            ui.onNodeWithTag("plugin-execution-allow").performScrollTo().performClick().assertIsOn()
            ui.onNodeWithTag(field).performScrollTo().performTextReplacement(value)
            Espresso.closeSoftKeyboard()
            ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        }
        ui.onNodeWithTag("plugin-execution-allow").performClick().assertIsOn()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle {
            assertNotNull("Production Node form must submit a complete pinned selection", launched?.execution)
            assertEquals(node(), launched?.execution?.runtime)
            assertEquals("/t", launched?.execution?.temporaryDirectory)
            assertEquals(true, launched?.execution?.authorized)
        }
        ui.runOnIdle { commandPath.value = "another" }
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("Submitted command must retain its original identity", "fixture", launched?.command) }
        ui.onNodeWithTag("plugin-execution-allow").performClick().assertIsOn()
        ui.onNodeWithTag("plugin-execution-tmpdir").performScrollTo().performTextReplacement("/changed")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("Submitted execution snapshot must retain the original directory", "/t", launched?.execution?.temporaryDirectory) }
        ui.onNodeWithTag("plugin-execution-allow").performClick().assertIsOn()
        choose("Exec")
        ui.onNodeWithTag("plugin-execution-allow").performScrollTo().assertIsOff()
        choose("Portable JS")
        ui.onNodeWithTag("plugin-execution-allow").assertDoesNotExist()
    }
    @Test fun nativePublicationRequiresClientResourceServiceAndWellFormedAbsolutePath() {
        val capabilities = listOf("resource.receive")
        val valid = ExtensionController.ClientWorkspace("/本机/quoted'😀", true)
        valid.validate(capabilities)
        assertEquals("Native 只读发布目录：/本机/quoted'😀 · 单文件上限 256 KiB", valid.reviewDescription)
        for (required in listOf(emptyList(), listOf("file.readScoped"))) rejected { valid.validate(required) }
        rejected { valid.copy(authorized = false).validate(capabilities) }
        for (path in listOf("", "relative", "/bad\npath", "/bad\rpath", "/bad\u0000path", "/\uD800", "/" + "界".repeat(1366))) {
            rejected { valid.copy(directory = path).validate(capabilities) }
        }
    }
    @Test fun localClientFormCapturesSeparateGrantAndRevokesItOnPathAndPackageEdits() {
        var launched: ExtensionController.ClientWorkspace? = null
        var request: ExtensionRelayBootstrap.Request? = null
        val selectedDigest = mutableStateOf(digest)
        ui.setContent {
            val command = ExtensionController.Command("fixture", "Fixture", "schemas/input.json", emptyList())
            val review = ExtensionController.Review("org.ghosterm.fixture", "0.1.0", "fixture", "MIT", selectedDigest.value,
                "portable-js", listOf("resource.receive"), emptyList(), false, false, listOf(command))
            MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
                listOf("fixture"), {}, { _, remote, local -> request = remote; launched = local }) }
        }
        fillCommon()
        ui.onNodeWithTag("plugin-client-workspace").performScrollTo().performTextInput("/local/原件")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle { assertNull(request) }
        ui.onNodeWithTag("plugin-client-workspace-allow").performScrollTo().assertIsOff().performClick()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle {
            assertEquals(ExtensionController.ClientWorkspace("/local/原件", true), launched)
            assertFalse(ExtensionRelayBootstrap.build(checkNotNull(request)).contains("/local/原件"))
            assertNull(request?.workspaceRootDirectory)
        }
        ui.onNodeWithTag("plugin-client-workspace").performScrollTo().performTextReplacement("/local/changed")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-client-workspace-allow").performScrollTo().assertIsOff().performClick()
        ui.runOnIdle { selectedDigest.value = "b".repeat(64) }
        ui.onNodeWithTag("plugin-client-workspace-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("/local/原件", launched?.directory) }
    }
    private fun form(commandPath: State<String>? = null, onLaunch: (ExtensionRelayBootstrap.Request) -> Unit) {
        ui.setContent {
            val command = ExtensionController.Command(commandPath?.value ?: "fixture", "Fixture", "schemas/input.json", emptyList())
            val review = ExtensionController.Review("org.ghosterm.fixture", "0.1.0", "fixture", "MIT", digest,
                "portable-js", emptyList(), emptyList(), false, false, listOf(command))
            MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
                listOf("fixture"), {}, { _, request, _ -> onLaunch(request) }) }
        }
    }
    private fun choose(choice: String) {
        ui.onNodeWithTag("plugin-executor-runtime").performScrollTo().performClick()
        ui.onNodeWithText(choice, useUnmergedTree = true).performClick()
    }
    private fun fillCommon() {
        ui.onNodeWithTag("plugin-executor-directory").performScrollTo().performTextInput("/k")
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextInput(digest)
        Espresso.closeSoftKeyboard()
    }
}

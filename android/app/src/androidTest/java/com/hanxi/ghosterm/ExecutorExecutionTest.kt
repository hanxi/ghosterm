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
    private fun request(environment: List<String> = emptyList()) = ExtensionRelayBootstrap.Request(
        "gterm", "/k", digest, digest, emptyList(), command = "fixture", processEnvironmentNames = environment)
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
    @Test fun productionLuaFormPreservesVariantAndHidesRetiredRuntimeControls() {
        var launched: ExtensionRelayBootstrap.Request? = null
        form { launched = it }
        fillCommon()
        ui.onNodeWithTag("plugin-executor-runtime").assertDoesNotExist()
        ui.onNodeWithTag("plugin-executor-variant").performScrollTo().performTextInput("lua")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithText("连接并请求授权").performClick()
        ui.runOnIdle {
            assertEquals("lua", checkNotNull(launched).variantID)
        }
    }
    @Test fun luaDefaultHasNoProcessExecutionFlags() {
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
        ui.runOnIdle { assertEquals("b".repeat(32),launched?.attachment?.instance);assertEquals(true,launched?.attachment?.authorized);assertEquals("",launched?.executorDirectory) }
        ui.onNodeWithTag("plugin-ipc-instance").performScrollTo().performTextReplacement("c".repeat(32))
        Espresso.closeSoftKeyboard();ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff()
        ui.runOnIdle { assertEquals("b".repeat(32),launched?.attachment?.instance) }
        ui.onNodeWithTag("plugin-ipc-allow").performClick().assertIsOn()
        ui.onNodeWithTag("plugin-ipc-processes").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-ipc-allow").performScrollTo().assertIsOff()
    }
    @Test fun environmentSelectionRequiresProcessMapping() {
        rejected { ExtensionRelayBootstrap.build(request(environment = listOf("FIXTURE_ROOT"))) }
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
                "lua", listOf("resource.receive"), emptyList(), false, false, listOf(command))
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
                "lua", emptyList(), emptyList(), false, false, listOf(command))
            MaterialTheme { PluginLaunchDialog(ExtensionPackagesViewModel.Entry(review), command,
                listOf("fixture"), {}, { _, request, _ -> onLaunch(request) }) }
        }
    }

    private fun fillCommon() {
        ui.onNodeWithTag("plugin-executor-directory").performScrollTo().performTextInput("/k")
        ui.onNodeWithTag("plugin-executor-digest").performScrollTo().performTextInput(digest)
        Espresso.closeSoftKeyboard()
    }
}

package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ClaudeObservationUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun fixture(block: (ClaudeConversation, ClaudeObservationFixture, () -> Unit) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val fake = ClaudeObservationFixture()
        var owner = true; lateinit var model: ClaudeConversation
        ui.runOnIdle { val client = ClaudeRPCClient(fake::rpc); model = ClaudeConversation(scope, client::request, { owner }, client::observe) }
        ui.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) { ClaudeConversationContent(model) } } }
        try { block(model, fake) { owner = false } } finally { ui.runOnIdle { model.invalidate(); fake.release.complete(Unit); scope.cancel() } }
    }
    private fun unknown(model: ClaudeConversation) {
        ui.runOnIdle { model.editDraft("保留未确认草稿 中文😀"); model.start() }
        ui.waitUntil(5000) { model.state.value.phase == ClaudeConversation.Phase.UNKNOWN }
    }
    @Test fun observationNeverResumesOrClearsTheDraft() = fixture { model, fake, _ ->
        unknown(model); val issue = model.state.value.issue
        ui.runOnIdle { model.observeOutcome() }; ui.waitUntil(5000) { model.state.value.observation != null }
        ui.runOnIdle { model.send(); model.start(); model.stop(); model.interrupt() }
        ui.runOnIdle {
            assertArrayEquals(fake.body, model.state.value.observation!!.response)
            assertEquals(ClaudeConversation.Phase.UNKNOWN, model.state.value.phase)
            assertEquals("保留未确认草稿 中文😀", model.state.value.draft); assertEquals(issue, model.state.value.issue)
            assertTrue(!model.canStart && !model.canSend && !model.canInterrupt && !model.canStop && fake.calls.count { it == "claude.call" } == 1)
        }
    }
    @Test fun detachedOrReplacedOwnerRejectsLateObservation() = fixture { model, fake, replace ->
        unknown(model)
        ui.runOnIdle { fake.pause = "claude.status"; model.observeOutcome() }
        ui.waitUntil(5000) { fake.entered.isCompleted }
        ui.runOnIdle { replace(); fake.release.complete(Unit) }
        ui.waitUntil(5000) { !model.state.value.busy }
        ui.runOnIdle { assertTrue("Late owner cannot publish an observation", model.state.value.observation == null && !model.canObserve); model.invalidate() }
    }
    @Test fun observationFailureKeepsTheOriginalIssue() = fixture { model, fake, _ ->
        unknown(model); val issue = model.state.value.issue
        ui.runOnIdle { fake.statusInput = "input-999"; model.observeOutcome() }
        ui.waitUntil(5000) { model.state.value.observationIssue != null }
        ui.runOnIdle { assertTrue(model.state.value.issue == issue && model.state.value.phase == ClaudeConversation.Phase.UNKNOWN && model.state.value.observation == null) }
    }
    @Test fun actualObserveButtonShowsCompleteSelectableReadonlyReceipt() = fixture { model, fake, _ ->
        unknown(model)
        ui.onNodeWithTag("plugin-claude-observe").performScrollTo().performClick()
        try { ui.waitUntil(5000) { model.state.value.observation != null } }
        catch (_: ComposeTimeoutException) { assertNotNull("Observe button must read the retained receipt", model.state.value.observation) }
        ui.onNodeWithTag("plugin-claude-observed-response").performScrollTo().assertTextEquals(fake.body.toString(Charsets.UTF_8))
            .assert(SemanticsMatcher.keyNotDefined(SemanticsActions.SetText))
            .performSemanticsAction(SemanticsActions.SetSelection) { assertTrue(it(0, 8, false)) }
        ui.onNodeWithTag("plugin-claude-send").performScrollTo().assertIsNotEnabled()
        ui.runOnIdle { assertTrue(model.state.value.phase == ClaudeConversation.Phase.UNKNOWN && fake.calls.count { it == "claude.call" } == 1) }
    }
}

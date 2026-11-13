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
import androidx.test.espresso.Espresso
import androidx.test.ext.junit.runners.AndroidJUnit4
import kotlinx.coroutines.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ClaudeConversationUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private fun fixture(block: (ClaudeConversation, ClaudeChatFixture) -> Unit) {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val fake = ClaudeChatFixture()
        lateinit var model: ClaudeConversation
        ui.runOnIdle { model = ClaudeConversation(scope, fake::request) }
        ui.setContent { MaterialTheme { Column(Modifier.verticalScroll(rememberScrollState())) { ClaudeConversationContent(model) } } }
        try { block(model, fake) } finally { ui.runOnIdle { model.invalidate(); fake.release(); scope.cancel() } }
    }
    private fun ready(model: ClaudeConversation) {
        ui.onNodeWithTag("plugin-claude-start").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.phase == ClaudeConversation.Phase.READY }
        ui.onNodeWithTag("plugin-claude-message").performScrollTo().performTextInput("explicit Native fixture")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-claude-send").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.phase == ClaudeConversation.Phase.RUNNING }
    }

    @Test fun realStartSendAndToolButtonsApproveTheImmutableInputOnce() = fixture { model, fake ->
        ready(model); ui.runOnIdle { fake.prompt() }; ui.waitUntil(5000) { model.state.value.pending.size == 1 }
        val prompt = model.state.value.pending.single()
        ui.onNodeWithTag("plugin-claude-tool-input").performScrollTo().assertTextEquals(prompt.inputJSON)
            .assert(SemanticsMatcher.keyNotDefined(SemanticsActions.SetText))
        ui.runOnIdle { fake.decisionEntered = CompletableDeferred(); fake.decisionRelease = CompletableDeferred() }
        ui.onNodeWithTag("plugin-claude-approval-allow").performScrollTo().performClick()
        ui.waitUntil(5000) { fake.decisionEntered!!.isCompleted }
        ui.onNodeWithTag("plugin-claude-approval-allow").assertIsNotEnabled()
        ui.onNodeWithTag("plugin-claude-approval-deny").assertIsNotEnabled()
        ui.runOnIdle { fake.decisionRelease!!.complete(Unit) }
        ui.waitUntil(5000) { model.state.value.pending.isEmpty() }
        ui.runOnIdle {
            val call = fake.calls.single { it.method == "approve" }
            assertTrue("Native allow button must send allow", call.input.getBoolean("allow"))
            assertFalse(call.input.has("input"))
        }
    }

    @Test fun everyQuestionStartsUnselectedAndFreeTextRequiresAnExplicitAnswer() = fixture { model, fake ->
        ready(model); ui.runOnIdle { fake.prompt(questions = JSONArray().put(fake.question("Choose format", multi = true))) }
        ui.waitUntil(5000) { model.state.value.pending.size == 1 }
        ui.onNodeWithTag("plugin-claude-approval-allow").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithTag("plugin-claude-question-0-option-0").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-claude-question-0-option-1").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-claude-approval-allow").performScrollTo().assertIsEnabled()
        ui.onNodeWithTag("plugin-claude-question-0-free").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-claude-approval-allow").performScrollTo().assertIsNotEnabled()
        ui.onNodeWithTag("plugin-claude-question-0-text").performScrollTo().performTextInput("自己的回答 中文😀")
        Espresso.closeSoftKeyboard()
        ui.onNodeWithTag("plugin-claude-approval-allow").performScrollTo().performClick()
        ui.waitUntil(5000) { model.state.value.pending.isEmpty() }
        ui.runOnIdle {
            assertEquals("自己的回答 中文😀", fake.calls.single { it.method == "answer" }.input.getJSONObject("answers").getString("Choose format"))
            assertFalse(fake.calls.any { it.method == "approve" })
        }
    }

    @Test fun retainedMessagesExposeAllLinesInSelectableReadOnlyText() = fixture { model, fake ->
        ready(model)
        val text = (1..1000).joinToString("\n") { "+$it 中文😀" } + "\n+LAST-LINE"
        ui.runOnIdle { fake.message(JSONObject().put("type", "assistant").put("message", JSONObject().put("content",
            JSONArray().put(JSONObject().put("type", "text").put("text", text))))) }
        ui.waitUntil(5000) { model.state.value.entries.any { it.text == text } }
        ui.onNodeWithTag("plugin-claude-entry-sdk:1-toggle").performScrollTo().performClick()
        val field = ui.onNodeWithTag("plugin-claude-entry-sdk:1")
        field.performScrollTo().assertTextEquals(text).assert(SemanticsMatcher.keyNotDefined(SemanticsActions.SetText))
        field.performSemanticsAction(SemanticsActions.SetSelection) { assertTrue(it(0, 8, false)) }
    }
}

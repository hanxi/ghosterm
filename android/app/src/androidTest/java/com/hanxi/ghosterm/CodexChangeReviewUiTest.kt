package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CodexChangeReviewUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()

    @Test fun fullDiffIsSelectableReadOnlyAndLastFileIsReachable() {
        val diff = (1..600).joinToString("\n") { "+$it 中文😀" } + "\n+last-line\n"
        val files = (0..9).map { CodexCompleteChangeFile("src/file-$it.kt", "update", null, if (it == 9) diff else "first-$it") }
        ui.setContent { MaterialTheme { CodexCompleteChanges(CodexChangeDocument("t", "u", "i", files)) } }
        ui.onNodeWithTag("plugin-codex-full-diff-text").assertTextEquals("first-0")
        ui.onNodeWithTag("plugin-codex-change-files").performScrollToNode(hasTestTag("plugin-codex-change-file-9"))
        ui.onNodeWithTag("plugin-codex-change-file-9").performClick()
        val text = ui.onNodeWithTag("plugin-codex-full-diff-text")
        text.assertTextEquals(diff)
        text.performSemanticsAction(SemanticsActions.GetTextLayoutResult) { layout ->
            val results = mutableListOf<TextLayoutResult>()
            assertTrue(layout(results))
            assertEquals(602, results.single().lineCount)
            assertFalse(results.single().isLineEllipsized(600))
        }
        text.assert(SemanticsMatcher.keyNotDefined(SemanticsActions.SetText))
        text.performSemanticsAction(SemanticsActions.SetSelection) { select -> assertTrue(select(0, 8, false)) }
    }

    @Test fun replacingDocumentShowsExactUnicodeAndResetsFileSelection() {
        fun document(first: String) = CodexChangeDocument("t", "u", "i", listOf(
            CodexCompleteChangeFile("é.kt", "add", null, first), CodexCompleteChangeFile("last.kt", "delete", null, "last")))
        val state = mutableStateOf(document("+é"))
        ui.setContent { MaterialTheme { CodexCompleteChanges(state.value) } }
        ui.onNodeWithTag("plugin-codex-full-diff-text").assertTextEquals("+é")
        ui.onNodeWithTag("plugin-codex-change-file-1").performClick()
        ui.onNodeWithTag("plugin-codex-full-diff-text").assertTextEquals("last")
        ui.runOnIdle { state.value = document("+e\u0301") }
        ui.onNodeWithTag("plugin-codex-full-diff-text").assertTextEquals("+e\u0301")
    }
}

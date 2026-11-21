package com.hanxi.ghosterm

import android.app.UiAutomation
import android.accessibilityservice.AccessibilityServiceInfo
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTagsAsResourceId
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import org.junit.Assert.assertTrue
import org.junit.Assert.assertFalse
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
@OptIn(ExperimentalComposeUiApi::class)
class PluginFormTalkBackTest {
    private fun automation(): UiAutomation {
        assumeTrue(InstrumentationRegistry.getArguments().getString("pluginTalkBack") == "true")
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val automation = instrumentation.getUiAutomation(UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES)
        automation.serviceInfo = automation.serviceInfo.apply {
            flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS or
                AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS
        }
        return automation
    }

    @Test fun presentRequestAndSubmissionStatus() {
        val automation = automation()
        val answering = mutableStateOf(false)
        val form = PluginForm.decode("""
            {"requestId":"42","title":"TalkBack form","fields":[
              {"kind":"text","id":"name","label":"Display name","required":true,"maxLength":32}
            ]}
        """.trimIndent().toByteArray())
        ActivityScenario.launch(ComponentActivity::class.java).use { scenario ->
            scenario.onActivity { activity ->
                activity.setContent {
                    MaterialTheme {
                        Column(Modifier.semantics { testTagsAsResourceId = true }) {
                            PluginFormContent(form, "org.ghosterm.talkback", answering.value) { answer ->
                                assertEquals(PluginForm.Answer.Submitted(mapOf("name" to PluginViewValue.Value.Text("example"))), answer)
                                answering.value = true
                            }
                        }
                    }
                }
            }
            val request = awaitText(automation, "插件 org.ghosterm.talkback 请求输入")
            assertTrue(request.performAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS))
            awaitSpeech(automation, "插件 org.ghosterm.talkback 请求输入")
            assertFalse(awaitNode(automation, "plugin-form-submit") { it.viewIdResourceName == "plugin-form-submit" }.isEnabled)
            enterText(automation, "plugin-form-field-name", "example")
            click(automation, "plugin-form-submit")
            awaitText(automation, "正在发送表单答复…")
            awaitSpeech(automation, "正在发送表单答复")
        }
    }

    @Test fun speakTableSearchProgressAndResults() {
        val automation = automation()
        val fixture = PluginTableFixture()
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        try {
            ActivityScenario.launch(ComponentActivity::class.java).use { scenario ->
                scenario.onActivity { activity ->
                    fixture.description.put("total", 1)
                    val model = PluginTableModel(scope, fixture::request, supportsSearch = true)
                    activity.setContent {
                        MaterialTheme {
                            Column(Modifier.semantics { testTagsAsResourceId = true }) {
                                PluginTableContent(model)
                            }
                        }
                    }
                }
                awaitText(automation, "第 1–1 行，共 1 行")
                assertFalse(awaitNode(automation, "plugin-table-search") { it.viewIdResourceName == "plugin-table-search" }.isEnabled)
                scenario.onActivity { fixture.pause = PluginTableModel.searchMethod }
                enterText(automation, "plugin-table-search-query", "Number")
                click(automation, "plugin-table-search")
                awaitText(automation, "正在搜索表格…")
                awaitSpeech(automation, "正在搜索表格")
                fixture.release.complete(Unit)
                awaitText(automation, "本页没有匹配项，可继续搜索")
                awaitSpeech(automation, "本页没有匹配项")
                click(automation, "plugin-table-search-more")
                awaitText(automation, "本页有 1 个匹配项")
                awaitSpeech(automation, "本页有 1 个匹配项")
                scenario.onActivity {
                    val (method, input) = fixture.calls.last()
                    assertEquals(PluginTableModel.searchMethod, method)
                    assertEquals("Number", input.getString("query"))
                    assertEquals(128L, input.getLong("after"))
                }
                click(automation, "plugin-table-search-clear")
                awaitText(automation, "第 1–1 行，共 1 行")
            }
        } finally {
            scope.cancel()
        }
    }

    private fun enterText(automation: UiAutomation, id: String, value: String) {
        val input = awaitNode(automation, id) { it.isEditable && it.viewIdResourceName == id }
        val arguments = Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, value)
        }
        assertTrue("Set text in $id", input.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, arguments))
    }

    private fun click(automation: UiAutomation, id: String) {
        val button = awaitNode(automation, "enabled $id") {
            it.isEnabled && it.isClickable && it.viewIdResourceName == id
        }
        assertTrue("Click $id", button.performAction(AccessibilityNodeInfo.ACTION_CLICK))
    }

    private fun awaitText(automation: UiAutomation, text: String): AccessibilityNodeInfo =
        awaitNode(automation, text) { it.text?.toString() == text }

    private fun awaitNode(automation: UiAutomation, description: String,
                          matches: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo {
        fun find(node: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (node == null) return null
            if (matches(node)) return node
            repeat(node.childCount) { index ->
                find(node.getChild(index))?.let { return it }
            }
            return null
        }
        repeat(100) {
            find(automation.rootInActiveWindow)?.let { return it }
            automation.windows.forEach { window -> find(window.root)?.let { return it } }
            Thread.sleep(100)
        }
        val windows = automation.windows.map { window -> "${window.title} (${window.root?.packageName})" }
        error("Missing accessibility node for $description in $windows")
    }

    private fun awaitSpeech(automation: UiAutomation, text: String) {
        val fragment = Regex("""SpeechControllerImpl: Speaking fragment text="([^"]+)", utteranceId=(\S+),""")
        repeat(40) {
            val spoken = ParcelFileDescriptor.AutoCloseInputStream(
                automation.executeShellCommand("logcat -b main -d -t 4000")
            ).bufferedReader().use { reader ->
                val utterances = mutableSetOf<String>()
                reader.lineSequence().any { line ->
                    val match = fragment.find(line)
                    if (match != null && match.groupValues[1].startsWith(text)) {
                        utterances.add(match.groupValues[2])
                        false
                    } else {
                        utterances.any { id -> line.contains("Speak call for $id returned 0") }
                    }
                }
            }
            if (spoken) return
            Thread.sleep(500)
        }
        error("TalkBack did not hand this speech to TTS: $text")
    }
}

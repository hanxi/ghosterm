package com.hanxi.ghosterm

import android.app.UiAutomation
import android.accessibilityservice.AccessibilityServiceInfo
import android.os.ParcelFileDescriptor
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginFormTalkBackTest {
    private fun automation(): UiAutomation {
        assumeTrue(InstrumentationRegistry.getArguments().getString("pluginTalkBack") == "true")
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val automation = instrumentation.getUiAutomation(UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES)
        automation.serviceInfo = automation.serviceInfo.apply {
            flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
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
                        PluginFormContent(form, "org.ghosterm.talkback", answering.value) { }
                    }
                }
            }
            val request = awaitText(automation, "插件 org.ghosterm.talkback 请求输入")
            assertTrue(request.performAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS))
            awaitSpeech(automation, "插件 org.ghosterm.talkback 请求输入")
            scenario.onActivity { answering.value = true }
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
                lateinit var model: PluginTableModel
                scenario.onActivity { activity ->
                    fixture.description.put("total", 1)
                    model = PluginTableModel(scope, fixture::request, supportsSearch = true)
                    activity.setContent { MaterialTheme { PluginTableContent(model) } }
                }
                awaitText(automation, "第 1–1 行，共 1 行")
                scenario.onActivity {
                    fixture.pause = PluginTableModel.searchMethod
                    checkNotNull(model.search("Number"))
                }
                awaitText(automation, "正在搜索表格…")
                awaitSpeech(automation, "正在搜索表格")
                fixture.release.complete(Unit)
                awaitText(automation, "本页没有匹配项，可继续搜索")
                awaitSpeech(automation, "本页没有匹配项")
                scenario.onActivity { checkNotNull(model.continueSearch()) }
                awaitText(automation, "本页有 1 个匹配项")
                awaitSpeech(automation, "本页有 1 个匹配项")
            }
        } finally {
            scope.cancel()
        }
    }

    private fun awaitText(automation: UiAutomation, text: String): AccessibilityNodeInfo {
        fun find(node: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (node == null) return null
            if (node.text?.toString() == text) return node
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
        error("Missing accessibility node for $text in $windows")
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

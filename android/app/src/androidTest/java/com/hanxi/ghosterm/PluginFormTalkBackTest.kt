package com.hanxi.ghosterm

import android.app.UiAutomation
import android.accessibilityservice.AccessibilityServiceInfo
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginFormTalkBackTest {
    @Test fun presentRequestAndSubmissionStatus() {
        assumeTrue(InstrumentationRegistry.getArguments().getString("pluginTalkBack") == "true")
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val automation = instrumentation.getUiAutomation(UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES)
        automation.serviceInfo = automation.serviceInfo.apply {
            flags = flags or AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
        }
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
            // Give the external screen reader time to consume the focus event before the live update.
            Thread.sleep(2_500)
            scenario.onActivity { answering.value = true }
            awaitText(automation, "正在发送表单答复…")
            Thread.sleep(2_500)
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
}

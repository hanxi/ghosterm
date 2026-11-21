package com.hanxi.ghosterm

import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PluginFormUiTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()

    private val schema = """
        {"requestId":"42","title":"Sample form","fields":[
          {"kind":"text","id":"name","label":"Display name","required":true,"maxLength":3},
          {"kind":"toggle","id":"summary","label":"Show summary"},
          {"kind":"choice","id":"layout","label":"Layout","options":[
            {"id":"compact","label":"Compact"},{"id":"comfortable","label":"Comfortable"}
          ]}
        ]}
    """.trimIndent()

    @Test fun nativeControlsValidateAndSubmitTypedValues() {
        val form = PluginForm.decode(schema.toByteArray())
        val busy = mutableStateOf(false)
        val replies = mutableListOf<PluginForm.Answer>()
        ui.setContent { MaterialTheme { PluginFormContent(form, "org.ghosterm.form", busy.value, replies::add) } }
        val input = ui.onNodeWithTag("plugin-form-field-name")
        val submit = ui.onNodeWithTag("plugin-form-submit")
        val status = ui.awaitNativePluginLiveRegion("插件 org.ghosterm.form 请求输入")
        submit.assertIsNotEnabled()
        input.performTextInput("甲乙丙丁")
        ui.onNodeWithTag("plugin-form-field-layout").performClick()
        ui.onNodeWithText("Compact").performClick()
        submit.assertIsNotEnabled()
        input.performTextReplacement("甲乙丙")
        ui.onNodeWithTag("plugin-form-field-summary").assertContentDescriptionEquals("Show summary").performClick()
        submit.assertIsEnabled().performClick()
        ui.runOnIdle {
            val reply = JSONObject(form.outcome(replies.single()).decodeToString()).getJSONObject("value")
            assertEquals("42", reply.getString("requestId"))
            assertEquals("submitted", reply.getString("kind"))
            val values = reply.getJSONObject("values")
            assertEquals("甲乙丙", values.getString("name"))
            assertTrue(values.getBoolean("summary"))
            assertEquals("compact", values.getString("layout"))
            busy.value = true
        }
        submit.assertIsNotEnabled()
        ui.onNodeWithTag("plugin-form-dismiss").assertIsNotEnabled()
        assertEquals(status, ui.awaitNativePluginLiveRegion("正在发送表单答复…"))
    }

    @Test fun dismissReturnsOnlyTheOriginalRequestId() {
        val form = PluginForm.decode(schema.toByteArray())
        val replies = mutableListOf<PluginForm.Answer>()
        ui.setContent { MaterialTheme { PluginFormContent(form, "org.ghosterm.form", false, replies::add) } }
        ui.onNodeWithTag("plugin-form-field-name").performTextInput("草稿")
        ui.onNodeWithTag("plugin-form-dismiss").performClick()
        ui.runOnIdle {
            val reply = JSONObject(form.outcome(replies.single()).decodeToString()).getJSONObject("value")
            assertEquals("42", reply.getString("requestId"))
            assertEquals("dismissed", reply.getString("kind"))
            assertFalse(reply.has("values"))
        }
    }
}

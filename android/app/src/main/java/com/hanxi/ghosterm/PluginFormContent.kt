package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.hanxi.ghosterm.PluginViewValue.Value

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun PluginFormContent(form: PluginForm, pluginName: String, answering: Boolean,
    answer: (PluginForm.Answer) -> Unit) {
    val values = remember(form) { mutableStateMapOf<String, Value>() }
    val supplied = form.fields.mapNotNull { field ->
        val value = values[field.id] ?: when (field.kind) {
            is PluginForm.Kind.Text -> Value.Text("")
            PluginForm.Kind.Toggle -> Value.Bool(false)
            is PluginForm.Kind.Choice -> null
        }
        value?.let { field.id to it }
    }.toMap()
    val submission = PluginForm.Answer.Submitted(supplied)
    Column(Modifier.fillMaxWidth().testTag("plugin-form"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("插件 $pluginName 请求输入", style = MaterialTheme.typography.bodySmall)
        Text(form.title, Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
        form.fields.forEach { field ->
            when (val kind = field.kind) {
                is PluginForm.Kind.Text -> OutlinedTextField(
                    value = (values[field.id] as? Value.Text)?.value.orEmpty(),
                    onValueChange = { values[field.id] = Value.Text(it) },
                    label = { Text(if (kind.required) "${field.label}（必填）" else field.label) },
                    singleLine = true, enabled = !answering,
                    modifier = Modifier.fillMaxWidth().testTag("plugin-form-field-${field.id}"))
                PluginForm.Kind.Toggle -> Row(horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    val checked = (values[field.id] as? Value.Bool)?.value ?: false
                    Checkbox(checked, { values[field.id] = Value.Bool(it) }, enabled = !answering,
                        modifier = Modifier.semantics { contentDescription = field.label }
                            .testTag("plugin-form-field-${field.id}"))
                    Text(field.label)
                }
                is PluginForm.Kind.Choice -> Box {
                    var expanded by remember(field.id) { mutableStateOf(false) }
                    val selected = (values[field.id] as? Value.Text)?.value
                    val label = kind.options.firstOrNull { it.id == selected }?.label ?: "请选择"
                    OutlinedButton(onClick = { expanded = true }, enabled = !answering,
                        modifier = Modifier.testTag("plugin-form-field-${field.id}")) { Text("${field.label} · $label") }
                    DropdownMenu(expanded && !answering, { expanded = false }) {
                        kind.options.forEach { option ->
                            DropdownMenuItem(text = { Text(option.label) }, onClick = {
                                values[field.id] = Value.Text(option.id)
                                expanded = false
                            })
                        }
                    }
                }
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { answer(submission) }, enabled = !answering && runCatching { form.outcome(submission) }.isSuccess,
                modifier = Modifier.testTag("plugin-form-submit")) { Text("提交") }
            TextButton(onClick = { answer(PluginForm.Answer.Dismissed) }, enabled = !answering,
                modifier = Modifier.testTag("plugin-form-dismiss")) { Text("取消表单") }
        }
    }
}

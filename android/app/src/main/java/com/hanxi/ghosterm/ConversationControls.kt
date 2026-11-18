package com.hanxi.ghosterm

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

/** Shared Native controls; each backend keeps its own decision/state contract. */
@Composable internal fun ConversationComposer(draft: String, onDraft: (String) -> Unit, busy: Boolean,
    canSend: Boolean, prefix: String, label: String, send: () -> Unit, enabled: Boolean = true) {
    OutlinedTextField(draft, onDraft, enabled = enabled, modifier = Modifier.fillMaxWidth().testTag("$prefix-message"),
        label = { Text(label) }, minLines = 1, maxLines = 4)
    TextButton(onClick = send, enabled = enabled && !busy && canSend && draft.isNotBlank(), modifier = Modifier.testTag("$prefix-send")) {
        Text(if (busy) "正在处理…" else "发送")
    }
}

@Composable internal fun ConversationDecisions(prefix: String, busy: Boolean, canAllow: Boolean,
    allowLabel: String = "批准一次", deny: () -> Unit, allow: () -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        TextButton(onClick = deny, enabled = !busy, modifier = Modifier.testTag("$prefix-deny")) { Text("拒绝") }
        TextButton(onClick = allow, enabled = !busy && canAllow, modifier = Modifier.testTag("$prefix-allow")) { Text(allowLabel) }
    }
}

@Composable internal fun ConversationReadOnlyText(text: String, tag: String) {
    Box(Modifier.fillMaxWidth().height(300.dp).verticalScroll(rememberScrollState()).horizontalScroll(rememberScrollState())) {
        BasicTextField(value = text, onValueChange = {}, readOnly = true,
            modifier = Modifier.widthIn(min = 280.dp).testTag(tag),
            textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace, color = MaterialTheme.colorScheme.onSurface))
    }
}

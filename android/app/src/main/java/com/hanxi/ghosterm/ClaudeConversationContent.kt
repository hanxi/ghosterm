package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.dp

@Composable internal fun ClaudeConversationContent(model: ClaudeConversation) {
    val state by model.state.collectAsState()
    Column(Modifier.fillMaxWidth().testTag("plugin-claude-panel"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Claude 对话", style = MaterialTheme.typography.titleSmall)
        Text(when (state.phase) {
            ClaudeConversation.Phase.NEW -> "尚未启动"
            ClaudeConversation.Phase.STARTING -> "正在初始化后端"
            ClaudeConversation.Phase.READY -> "后端已就绪；认证与模型调用结果需由实际请求确认"
            ClaudeConversation.Phase.RUNNING -> "正在处理当前输入"
            ClaudeConversation.Phase.INTERRUPTING -> "已请求中断，等待后端最终结果"
            ClaudeConversation.Phase.CLOSED -> "后端会话已关闭"
            ClaudeConversation.Phase.FAILED -> "后端会话失败"
            ClaudeConversation.Phase.LOST -> "后端连接已丢失，未确认结果保持未知"
            ClaudeConversation.Phase.UNKNOWN -> "无法确认当前会话状态"
        }, Modifier.testTag("plugin-claude-phase"), style = MaterialTheme.typography.bodySmall)
        state.entries.forEach { entry -> key(entry.id) {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(entry.title, style = MaterialTheme.typography.labelLarge)
                    SelectionContainer { Text(entry.text.codePoints().limit(8192).toArray().let { String(it, 0, it.size) }, maxLines = 8) }
                    if (entry.text.isNotEmpty()) ClaudeCompleteText("查看本条完整内容", entry.text, "plugin-claude-entry-${entry.id}")
                }
            }
        } }
        state.pending.forEach { prompt -> key(prompt.reference) { ClaudePrompt(model, prompt, model.canRespond(prompt.reference)) } }
        if (state.omittedEntries > 0) Text("较早的 ${state.omittedEntries} 条对话预览已超出本次内存保留范围。")
        if (state.phase == ClaudeConversation.Phase.NEW) {
            TextButton(onClick = model::start, enabled = model.canStart, modifier = Modifier.testTag("plugin-claude-start")) { Text("启动 Claude 对话") }
        } else {
            ConversationComposer(state.draft, model::editDraft, state.busy, model.canSend, "plugin-claude", "发送给 Claude", model::send)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = model::interrupt, enabled = model.canInterrupt, modifier = Modifier.testTag("plugin-claude-interrupt")) { Text("中断当前输入") }
                TextButton(onClick = model::stop, enabled = model.canStop, modifier = Modifier.testTag("plugin-claude-stop")) { Text("停止 Claude 会话") }
            }
        }
        state.issue?.let { Text(it, Modifier.testTag("plugin-claude-issue"), color = MaterialTheme.colorScheme.error) }
        if (state.phase == ClaudeConversation.Phase.UNKNOWN) {
            TextButton(onClick = model::observeOutcome, enabled = model.canObserve,
                modifier = Modifier.testTag("plugin-claude-observe")) { Text("查看未确认操作回包") }
            Text("仅查看本次连接中的回包；原操作保持未知。继续操作需停止并释放本次插件。", style = MaterialTheme.typography.bodySmall)
        }
        state.observation?.let { observed ->
            Text(when (observed.source) {
                ClaudeRPCClient.Observation.Source.UNAVAILABLE -> "尚未观察到可关联的完整回包；不代表操作未执行"
                ClaudeRPCClient.Observation.Source.EXECUTOR -> "已读取执行端保留回包；原操作确认仍未知"
                ClaudeRPCClient.Observation.Source.RECEIVED -> "本机已保留完整回包；原操作确认仍未知"
            }, Modifier.testTag("plugin-claude-observation-status"))
            observed.text?.let { ConversationReadOnlyText(it, "plugin-claude-observed-response") }
            ClaudeCompleteText("回包关联信息", "操作：${observed.method}\n输入：${observed.inputID ?: "未取得"}\n回包：${observed.responseID ?: "未取得"}\n" +
                "进程观察：${observed.backendPhase ?: "未取得"}\n诊断：${observed.failure.orEmpty()}", "plugin-claude-observation-metadata")
            Text("回包不代表模型任务已完成；不会据此重试或恢复对话。", style = MaterialTheme.typography.bodySmall)
        }
        state.observationIssue?.let { Text(it, Modifier.testTag("plugin-claude-observation-issue"), color = MaterialTheme.colorScheme.error) }
        if (state.records.isNotEmpty()) {
            var show by remember { mutableStateOf(false) }
            TextButton(onClick = { show = !show }, modifier = Modifier.testTag("plugin-claude-raw-toggle")) { Text("原始 Claude 记录（${state.records.size}）") }
            if (show) state.records.forEach { record -> key(record.id) {
                ClaudeCompleteText("查看完整记录 ${record.id}", record.json, "plugin-claude-record-${record.id}")
            } }
        }
        if (state.omittedRecords > 0) Text("较早的 ${state.omittedRecords} 条原始记录已超出本次内存保留范围。")
    }
}

@Composable private fun ClaudeCompleteText(label: String, text: String, tag: String) {
    var show by remember { mutableStateOf(false) }
    TextButton(onClick = { show = !show }, modifier = Modifier.testTag("$tag-toggle")) { Text(label) }
    if (show) ConversationReadOnlyText(text, tag)
}

private data class ClaudeAnswerDraft(val selected: Set<Int> = emptySet(), val freeText: Boolean = false, val text: String = "")
@Composable private fun ClaudePrompt(model: ClaudeConversation, prompt: ClaudeConversation.Prompt, enabled: Boolean) {
    var drafts by remember(prompt.reference) { mutableStateOf(prompt.questions.map { ClaudeAnswerDraft() }) }
    val answers = prompt.questions.mapIndexed { index, question ->
        val draft = drafts[index]
        if (draft.freeText) draft.text else question.options.mapIndexedNotNull { option, value -> value.label.takeIf { option in draft.selected } }.joinToString(", ")
    }.takeIf { values -> values.all { it.isNotEmpty() && ClaudeJson.utf8(it).size <= 2048 } }
    fun update(index: Int, value: ClaudeAnswerDraft) { drafts = drafts.mapIndexed { at, old -> if (at == index) value else old } }
    Card(Modifier.fillMaxWidth().testTag("plugin-claude-prompt-${prompt.reference.id}")) {
        Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(if (prompt.isQuestion) "Claude 需要你的回答" else "Claude 请求执行工具：${prompt.toolName}", style = MaterialTheme.typography.labelLarge)
            if (prompt.isQuestion) prompt.questions.forEachIndexed { index, question ->
                Text(question.header, style = MaterialTheme.typography.labelMedium)
                SelectionContainer { Text(question.question) }
                val draft = drafts[index]
                question.options.forEachIndexed { option, value ->
                    Row(Modifier.fillMaxWidth()) {
                        Checkbox(option in draft.selected, onCheckedChange = { checked ->
                            val selected = if (question.multiSelect) draft.selected else emptySet()
                            update(index, draft.copy(selected = if (checked) selected + option else selected - option))
                        }, enabled = enabled && !draft.freeText, modifier = Modifier.testTag("plugin-claude-question-$index-option-$option"))
                        Column { Text(value.label); Text(value.description, style = MaterialTheme.typography.bodySmall) }
                    }
                }
                Row {
                    Checkbox(draft.freeText, onCheckedChange = { update(index, draft.copy(freeText = it)) }, enabled = enabled,
                        modifier = Modifier.testTag("plugin-claude-question-$index-free"))
                    Text("填写自己的回答")
                }
                if (draft.freeText) OutlinedTextField(draft.text, { update(index, draft.copy(text = it)) }, enabled = enabled,
                    modifier = Modifier.fillMaxWidth().testTag("plugin-claude-question-$index-text"), label = { Text("你的回答") }, minLines = 1, maxLines = 4)
            } else ConversationReadOnlyText(prompt.inputJSON, "plugin-claude-tool-input")
            ClaudeCompleteText("完整请求信息", prompt.metadataJSON, "plugin-claude-prompt-metadata")
            ConversationDecisions("plugin-claude-approval", !enabled, !prompt.isQuestion || answers != null,
                if (prompt.isQuestion) "提交回答" else "批准一次", deny = { model.respond(prompt.reference, false) }, allow = {
                    if (prompt.isQuestion) answers?.let { model.answer(prompt.reference, it) }
                    else model.respond(prompt.reference, true)
                })
        }
    }
}

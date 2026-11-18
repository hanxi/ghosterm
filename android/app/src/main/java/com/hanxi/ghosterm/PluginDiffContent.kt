package com.hanxi.ghosterm

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

/** Standard read-only Native contribution, independent of provider or command. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun PluginDiffContent(model: PluginDiffModel) {
    val state by model.state.collectAsState()
    LaunchedEffect(model) { if (model.state.value.descriptor == null && model.state.value.issue == null) model.load()?.join() }
    Column(Modifier.fillMaxWidth().testTag("plugin-diff"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(state.descriptor?.title ?: "差异", Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
        state.issue?.let { Text(it, Modifier.testTag("plugin-diff-issue")
            .semantics { liveRegion = LiveRegionMode.Polite }, color = MaterialTheme.colorScheme.error) }
        if (state.busy) Text("正在读取差异…", Modifier.testTag("plugin-diff-loading"))
        val description = state.descriptor; val page = state.page
        if (description != null && page != null) {
            Text("${description.beforeLabel} → ${description.afterLabel}", Modifier.testTag("plugin-diff-labels"))
            Text("新增 ${description.added} 行 · 删除 ${description.removed} 行", Modifier.testTag("plugin-diff-summary"))
            if (description.total == 0) Text("两份文本均为空", Modifier.testTag("plugin-diff-empty"))
            else {
                if (description.added == 0 && description.removed == 0) Text("两份文本相同", Modifier.testTag("plugin-diff-identical"))
                Text("第 ${page.offset + 1}–${page.offset + page.rows.size} 行，共 ${description.total} 行", Modifier.testTag("plugin-diff-range"))
                // Independent scroll semantics keep both axes addressable.
                Box(Modifier.fillMaxWidth().heightIn(max = 320.dp).horizontalScroll(rememberScrollState()).testTag("plugin-diff-rows")) {
                    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        page.rows.forEachIndexed { index, row -> key(page.offset + index) {
                            Row(Modifier.padding(8.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                Text(when (row.kind) { PluginDiffRow.Kind.INSERT -> "+ 新增"; PluginDiffRow.Kind.DELETE -> "− 删除"; PluginDiffRow.Kind.EQUAL -> "= 相同" })
                                Text("${if (row.beforeLine == 0) "—" else row.beforeLine} / ${if (row.afterLine == 0) "—" else row.afterLine}", Modifier.semantics { contentDescription = "原 / 新行号" })
                                SelectionContainer {
                                    Text(row.text, Modifier.testTag("plugin-diff-text-${page.offset + index}").semantics {
                                        contentDescription = "${if (row.beforeLine == 0) "原文无此行" else "原第 ${row.beforeLine} 行"}，${if (row.afterLine == 0) "新文无此行" else "新第 ${row.afterLine} 行"}"
                                    }, fontFamily = FontFamily.Monospace)
                                }
                                Text(row.ending.label, Modifier.testTag("plugin-diff-ending-${page.offset + index}").semantics { contentDescription = "换行：${row.ending.label}" }, fontFamily = FontFamily.Monospace)
                            }
                        } }
                    }
                }
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { model.load() }, enabled = model.available && !state.busy, modifier = Modifier.testTag("plugin-diff-reload")) { Text("重新读取") }
            TextButton(onClick = { model.showPage(maxOf(0, state.selectedOffset - (description?.pageSize ?: 3))) }, enabled = model.canPrevious,
                modifier = Modifier.testTag("plugin-diff-previous")) { Text("上一页") }
            TextButton(onClick = { model.showPage(state.selectedOffset + (description?.pageSize ?: 3)) }, enabled = model.canNext,
                modifier = Modifier.testTag("plugin-diff-next")) { Text("下一页") }
        }
    }
}

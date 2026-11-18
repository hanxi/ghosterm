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
import androidx.compose.ui.unit.dp

/** Standard Native table contribution; no plugin-specific pane or WebView. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun PluginTableContent(model: PluginTableModel) {
    val state by model.state.collectAsState()
    LaunchedEffect(model) { if (model.state.value.descriptor == null && model.state.value.issue == null) model.load()?.join() }
    Column(Modifier.fillMaxWidth().testTag("plugin-table"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(state.descriptor?.title ?: "表格", Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
        state.issue?.let { Text(it, Modifier.testTag("plugin-table-issue")
            .semantics { liveRegion = LiveRegionMode.Polite }, color = MaterialTheme.colorScheme.error) }
        if (state.busy) Text("正在读取表格…", Modifier.testTag("plugin-table-loading"))
        val description = state.descriptor; val page = state.page
        if (description != null && page != null) {
            if (description.total == 0) Text("空表", Modifier.testTag("plugin-table-empty"))
            else {
                Text("第 ${page.offset + 1}–${page.offset + page.rows.size} 行，共 ${description.total} 行", Modifier.testTag("plugin-table-range"))
                // Keep one scroll action per semantics node. Combining both axes on
                // one node loses an axis for accessibility/bring-into-view actions.
                Box(Modifier.fillMaxWidth().heightIn(max = 320.dp).horizontalScroll(rememberScrollState())
                    .semantics { collectionInfo = CollectionInfo(page.rows.size + 1, description.columns.size) }
                    .testTag("plugin-table-grid")) {
                  Column(Modifier.verticalScroll(rememberScrollState())) {
                    Row {
                        description.columns.forEach { column ->
                            Text(column.title, Modifier.width(200.dp).padding(8.dp).semantics { heading() }
                                .testTag("plugin-table-column-${column.id}"), style = MaterialTheme.typography.titleSmall)
                        }
                    }
                    page.rows.forEachIndexed { index, row -> key(row.id) {
                        Row {
                            row.cells.forEachIndexed { column, value ->
                                SelectionContainer {
                                    Text(value, Modifier.width(200.dp).padding(8.dp).semantics {
                                        contentDescription = "${description.columns[column].title}，第 ${page.offset + index + 1} 行"
                                        collectionItemInfo = CollectionItemInfo(index + 1, 1, column, 1)
                                    }.testTag("plugin-table-cell-${row.id}-${description.columns[column].id}"))
                                }
                            }
                        }
                    } }
                  }
                }
            }
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            TextButton(onClick = { model.load() }, enabled = model.available && !state.busy, modifier = Modifier.testTag("plugin-table-reload")) { Text("重新读取") }
            TextButton(onClick = { model.showPage(maxOf(0, state.selectedOffset - (description?.pageSize ?: 16))) }, enabled = model.canPrevious,
                modifier = Modifier.testTag("plugin-table-previous")) { Text("上一页") }
            TextButton(onClick = { model.showPage(state.selectedOffset + (description?.pageSize ?: 16)) }, enabled = model.canNext,
                modifier = Modifier.testTag("plugin-table-next")) { Text("下一页") }
            TextButton(onClick = { model.showPage(model.lastOffset) }, enabled = model.canNext,
                modifier = Modifier.testTag("plugin-table-last")) { Text("末页") }
        }
    }
}

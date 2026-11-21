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
    var query by remember(model) { mutableStateOf("") }
    LaunchedEffect(model) { if (model.state.value.descriptor == null && model.state.value.issue == null) model.load()?.join() }
    Column(Modifier.fillMaxWidth().testTag("plugin-table"), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(state.descriptor?.title ?: "表格", Modifier.semantics { heading() }, style = MaterialTheme.typography.titleMedium)
        state.issue?.let { Text(it, Modifier.testTag("plugin-table-issue")
            .semantics { liveRegion = LiveRegionMode.Polite }, color = MaterialTheme.colorScheme.error) }
        val description = state.descriptor; val page = state.page
        val searchPage = state.searchPage
        val searching = state.searchQuery != null
        val rows = if (searching) searchPage?.rows else page?.rows
        val offset = if (searching) 0 else page?.offset ?: 0
        if (model.supportsSearch) {
            OutlinedTextField(query, { query = it }, label = { Text("搜索表格") }, singleLine = true,
                enabled = model.available, modifier = Modifier.fillMaxWidth().testTag("plugin-table-search-query"))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = { model.search(query) }, enabled = model.available && !state.busy &&
                    description != null && query.isNotEmpty() && query.codePointCount(0, query.length) <= 64,
                    modifier = Modifier.testTag("plugin-table-search")) { Text("搜索") }
                if (searchPage != null && searchPage.cursor < searchPage.latest) {
                    TextButton(onClick = { model.continueSearch() }, enabled = model.available && !state.busy,
                        modifier = Modifier.testTag("plugin-table-search-more")) { Text("继续搜索") }
                }
                if (searching) {
                    TextButton(onClick = { model.load() }, enabled = model.available && !state.busy,
                        modifier = Modifier.testTag("plugin-table-search-clear")) { Text("返回表格") }
                }
            }
            state.searchQuery?.let { Text("搜索：$it") }
            if (state.searchTruncated) Text("部分历史记录已超出保留范围")
        }
        val status = when {
            state.busy -> "正在读取表格…"
            searchPage != null && searchPage.rows.isNotEmpty() -> "本页有 ${searchPage.rows.size} 个匹配项"
            searchPage != null && searchPage.cursor < searchPage.latest -> "本页没有匹配项，可继续搜索"
            searchPage != null -> "搜索结束，本页没有匹配项"
            !searching && description != null && page != null && description.total > 0 ->
                "第 ${page.offset + 1}–${page.offset + page.rows.size} 行，共 ${description.total} 行"
            else -> null
        }
        // Keep the same live-region node while a page loads and its range replaces the progress text.
        if (status != null) Text(status, Modifier
            .testTag(if (state.busy) "plugin-table-loading" else "plugin-table-range")
            .semantics { liveRegion = LiveRegionMode.Polite })
        if (description != null && rows != null) {
            if (!searching && description.total == 0) Text("空表", Modifier.testTag("plugin-table-empty"))
            else if (rows.isNotEmpty()) {
                // Keep one scroll action per semantics node. Combining both axes on
                // one node loses an axis for accessibility/bring-into-view actions.
                Box(Modifier.fillMaxWidth().heightIn(max = 320.dp).horizontalScroll(rememberScrollState())
                    .semantics { collectionInfo = CollectionInfo(rows.size + 1, description.columns.size) }
                    .testTag("plugin-table-grid")) {
                  Column(Modifier.verticalScroll(rememberScrollState())) {
                    Row {
                        description.columns.forEach { column ->
                            Text(column.title, Modifier.width(200.dp).padding(8.dp).semantics { heading() }
                                .testTag("plugin-table-column-${column.id}"), style = MaterialTheme.typography.titleSmall)
                        }
                    }
                    rows.forEachIndexed { index, row -> key(row.id) {
                        Row {
                            row.cells.forEachIndexed { column, value ->
                                SelectionContainer {
                                    Text(value, Modifier.width(200.dp).padding(8.dp).semantics {
                                        contentDescription = "${description.columns[column].title}，第 ${offset + index + 1} 行，" +
                                            value.ifEmpty { "空白" }
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
        if (!searching) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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

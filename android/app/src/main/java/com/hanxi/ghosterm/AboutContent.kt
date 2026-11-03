package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp

@Composable
fun AboutContent(model: AboutViewModel, runtime: RuntimeInfo) {
    val context = LocalContext.current
    LaunchedEffect(model) { model.enter() }
    val pages = rememberSaveableStateHolder()
    pages.SaveableStateProvider(model.selected?.name ?: "overview") {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        val selected = model.selected
        if (selected != null) {
            when (val state = model.document) {
                AboutDocument.Loading -> Text("正在读取许可证…")
                AboutDocument.Failed -> {
                    Text("许可证读取失败。", Modifier.testTag("about-license-error"), color = MaterialTheme.colorScheme.error)
                    OutlinedButton({ model.open(selected) }, Modifier.heightIn(min = 48.dp)) { Text("重新读取许可证") }
                }
                is AboutDocument.Ready -> SelectionContainer { Text(state.text, Modifier.testTag("about-license-document"), fontFamily = FontFamily.Monospace) }
            }
        } else {
            Text("Ghosterm", style = MaterialTheme.typography.headlineMedium)
            Text("版本 ${BuildConfig.VERSION_NAME}", Modifier.testTag("about-version"))
            model.build?.let { info ->
                SelectionContainer { Text(info.commit ?: "无法确认提交", Modifier.testTag("about-commit"), fontFamily = FontFamily.Monospace) }
                Text(info.statusText, Modifier.testTag("about-source-state"))
            } ?: if (model.buildFailed) {
                Text("构建信息读取失败，无法确认提交。", Modifier.testTag("about-build-error"), color = MaterialTheme.colorScheme.error)
                TextButton(model::enter, Modifier.heightIn(min = 48.dp)) { Text("重新读取构建信息") }
            } else Text("正在读取构建信息…")
            when (runtime) {
                RuntimeInfo.Loading -> Text("正在加载终端组件…")
                RuntimeInfo.Failed -> Text("终端组件加载失败", color = MaterialTheme.colorScheme.error)
                is RuntimeInfo.Ready -> { Text("Core ${runtime.core}"); Text("libghostty-vt ${runtime.terminal}") }
            }
            Text("原生 Android 客户端 · MIT License")
            Text("项目与终端组件许可证", style = MaterialTheme.typography.titleMedium)
            AboutLicense.entries.forEach { license ->
                GhostermGroup { GhostermActionRow(license.title,icon=UiIcon.File,modifier=Modifier.testTag("about-license-${license.name}")) { model.open(license) } }
            }
            GhostermGroup { GhostermActionRow("项目源码",icon=UiIcon.Terminal,modifier=Modifier.testTag("about-source")) { model.openSource(context) } }
            SelectionContainer { Text(AboutViewModel.SOURCE) }
            model.linkError?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
    }
}

package com.hanxi.ghosterm

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

@Composable
internal fun PluginInstalledVersionsContent(controller: ExtensionController?, onChanged: (ExtensionController.VersionSelectionResult) -> Unit = {}, onRemoved: (PluginReleaseRemoval.Result) -> Unit = {}) {
    var expanded by remember(controller) { mutableStateOf(false) }
    TextButton(onClick = { expanded = !expanded }, modifier = Modifier.testTag("plugin-versions-disclosure")) {
        Text(if (expanded) "收起本机版本" else "查看已保存的本机版本…")
    }
    if (expanded && controller != null) PluginInstalledVersionsList(controller, onChanged, onRemoved)
}

@Composable
private fun PluginInstalledVersionsList(controller: ExtensionController, onChanged: (ExtensionController.VersionSelectionResult) -> Unit, onRemoved: (PluginReleaseRemoval.Result) -> Unit) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current.applicationContext
    val observationLock = remember(controller) { Mutex() }
    val changed = rememberUpdatedState(onChanged)
    val removed = rememberUpdatedState(onRemoved)
    val model = remember(controller) { PluginInstalledVersionsModel(scope) {
        observationLock.withLock {
            val worker = controller.newReleaseInspector()
            try { worker.installedVersions() } finally { withContext(NonCancellable) { worker.close() } }
        }
    } }
    val policyModel = remember(controller) { PluginPolicyReviewModel(scope, { id, policy -> controller.previewPolicyReview(id, policy) },
        { id, ids, policy -> controller.applyPolicyReview(id, ids, policy, true) }, controller::cancelPolicyReview,
        { result -> result.pluginIDs.forEach { id -> changed.value(ExtensionController.VersionSelectionResult(id, null, result.observation, result.durabilityConfirmed)) }; model.refresh() }) }
    val selectionModel = remember(controller) { PluginVersionSelectionModel(scope,
        { PluginSignatureDocuments.read(context.contentResolver, it, PluginSignatureMaterial.POLICY) },
        controller::previewInstalledVersion, { controller.confirmInstalledVersion(it, true) }, controller::cancelVersionPreview,
        { id, revision -> controller.disableInstalledVersion(id, revision, true) },
        { result -> changed.value(result); model.refresh() }, policyModel::setPolicy) }
    val maintenanceModel = remember(controller) { PluginInstallMaintenanceModel(scope,
        { observationLock.withLock {
            val inspector = controller.newReleaseInspector()
            try { inspector.installationMaintenance() } finally { withContext(NonCancellable) { inspector.close() } }
        } }, { controller.retireInstallationRecord(it, true) }, { model.refresh() }) }
    val unclassifiedModel = remember(controller) { PluginUnclassifiedModel(scope) {
        observationLock.withLock {
            val inspector = controller.newReleaseInspector()
            try { inspector.unclassifiedDirectories() } finally { withContext(NonCancellable) { inspector.close() } }
        }
    } }
    val removalModel = remember(controller) { PluginVersionRemovalModel(scope,
        { observationLock.withLock {
            val inspector = controller.newReleaseInspector()
            try { inspector.pendingVersionRemoval() } finally { withContext(NonCancellable) { inspector.close() } }
        } },
        controller::previewVersionRemoval, { controller.confirmVersionRemoval(it, true) }, controller::cancelRemovalPreview,
        { controller.resumeVersionRemoval(it, true) }, { controller.cancelPendingVersionRemoval(it, true) },
        { result -> result?.let { removed.value(it) }; model.refresh(); maintenanceModel.refresh(); unclassifiedModel.refresh() }, controller::previewStageRemoval,
        controller::previewUnclassifiedRemoval,
        { id, ack -> controller.confirmVersionRemoval(id, true, acknowledgedUnclassified = ack) },
        { id, ack -> controller.resumeVersionRemoval(id, true, acknowledgedUnclassified = ack) }) }
    DisposableEffect(unclassifiedModel) { onDispose { unclassifiedModel.close() } }
    DisposableEffect(maintenanceModel) { onDispose { maintenanceModel.close() } }
    DisposableEffect(removalModel) { onDispose { removalModel.close() } }
    DisposableEffect(model) { onDispose { model.close() } }
    DisposableEffect(policyModel) { onDispose { policyModel.close() } }
    DisposableEffect(selectionModel) { onDispose { selectionModel.close() } }
    LaunchedEffect(model) { model.refresh(); removalModel.refresh(); maintenanceModel.refresh(); unclassifiedModel.refresh() }
    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = { policyModel.cancel(); selectionModel.cancel(); removalModel.refresh(); maintenanceModel.refresh(); unclassifiedModel.refresh(); model.refresh() }, enabled = !policyModel.committing && !model.busy && !selectionModel.committing && !removalModel.committing && !maintenanceModel.committing,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-versions-refresh")) { Text("刷新版本列表") }
        if (model.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (removalModel.preview == null && !removalModel.busy) PluginVersionPolicyContent(selectionModel, enabled = !policyModel.committing)
        PluginPolicyReviewContent(policyModel, canAct = !selectionModel.busy && selectionModel.pending == null && !removalModel.busy && removalModel.preview == null && !maintenanceModel.committing,
            begin = { selectionModel.cancel(); removalModel.cancel(); maintenanceModel.cancel(); policyModel.inspect() })
        model.issue?.let { Text(it, Modifier.testTag("plugin-versions-issue"), color = MaterialTheme.colorScheme.error) }
        model.observation?.let { value ->
            Text("已保存版本 ${value.versions.size} 项", Modifier.testTag("plugin-versions-count"), style = MaterialTheme.typography.titleMedium)
            Text("仅显示已保存的声明，尚未用当前可信策略核验；不授权或执行插件。", Modifier.testTag("plugin-versions-unverified"), style = MaterialTheme.typography.bodySmall)
            value.selectionIssue?.let { Text(it, Modifier.testTag("plugin-versions-selection-unknown"), color = MaterialTheme.colorScheme.error) }
            val selected = value.selection
            if (selected != null && selected.choices.isEmpty()) Text("当前没有活动选择记录。", Modifier.testTag("plugin-versions-no-selection"))
            selected?.choices?.toSortedMap()?.forEach { (id, choice) ->
                Text("$id\n活动选择 ${choice.releaseID}\n${if (choice.enabled) "记录为启用" else "记录为禁用"}",
                    Modifier.testTag("plugin-versions-choice-$id"), style = MaterialTheme.typography.bodySmall)
                if (choice.enabled) OutlinedButton(onClick = { policyModel.cancel(); selectionModel.beginDisable(id, selected.revision) }, enabled = !policyModel.busy && policyModel.review == null && !selectionModel.busy && !removalModel.busy && removalModel.preview == null,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-disable-$id")) { Text("禁用此插件的后续新实例…") }
            }
            value.versions.forEach { version ->
                SelectionContainer { Column(Modifier.fillMaxWidth().testTag("plugin-version-${version.releaseID}"), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("${version.pluginID} ${version.version}", style = MaterialTheme.typography.titleSmall)
                    Text("声明发布者 ${version.publisher}")
                    Text("发布描述 SHA-256：${version.releaseID}\n目录包 SHA-256：${version.packageSHA256}", fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                    Text(when {
                        selected == null -> "活动选择状态查不出来"
                        selected.choices[version.pluginID]?.releaseID != version.releaseID -> "未选择此版本"
                        selected.choices.getValue(version.pluginID).enabled -> "活动记录选择此版本（启用）"
                        else -> "活动记录选择此版本（禁用）"
                    })
                } }
                OutlinedButton(onClick = { policyModel.cancel(); removalModel.cancel(); selectionModel.inspect(version.releaseID, selected?.revision) },
                    enabled = !policyModel.busy && policyModel.review == null && selectionModel.canPreview && selected != null && !model.busy && !removalModel.busy && removalModel.preview == null && removalModel.saved == null,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-select-${version.releaseID}")) { Text("核验并选择此版本…") }
                OutlinedButton(onClick = { policyModel.cancel(); selectionModel.cancel(); maintenanceModel.cancel(); removalModel.inspect(version.releaseID, selected?.revision) },
                    enabled = !policyModel.busy && policyModel.review == null && removalModel.canPreview && !selectionModel.busy && !model.busy && selected != null && selected.choices.values.none { it.enabled && it.releaseID == version.releaseID },
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("plugin-version-remove-${version.releaseID}")) { Text("核对并删除此本机版本…") }
            }
            value.issues.forEach { Text(it, Modifier.testTag("plugin-versions-entry-issue"), color = MaterialTheme.colorScheme.error) }
            if (value.maintenanceCount > 0) Text("安装暂存和未识别对象 ${value.maintenanceCount} 项。", Modifier.testTag("plugin-versions-maintenance"))
        }
        PluginInstallMaintenanceContent(maintenanceModel, removalModel,
            canAct = !policyModel.busy && policyModel.review == null && !selectionModel.busy && selectionModel.preview == null && selectionModel.disableTarget == null && !removalModel.busy && removalModel.preview == null && removalModel.saved == null,
            inspect = { policyModel.cancel(); selectionModel.cancel(); maintenanceModel.cancel(); removalModel.inspectStage(it) })
        PluginUnclassifiedContent(unclassifiedModel,
            canAct = !policyModel.busy && policyModel.review == null && removalModel.canPreview && !selectionModel.busy && selectionModel.preview == null && selectionModel.disableTarget == null && !maintenanceModel.busy && maintenanceModel.target == null,
            inspect = { policyModel.cancel(); selectionModel.cancel(); maintenanceModel.cancel(); removalModel.inspectUnclassified(it) })
        PluginVersionPreviewContent(selectionModel)
        PluginVersionRemovalContent(removalModel)
    }
}

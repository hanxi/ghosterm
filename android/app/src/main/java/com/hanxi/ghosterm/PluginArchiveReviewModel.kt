package com.hanxi.ghosterm

import android.net.Uri
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.*

internal enum class PluginArchiveMaterial(val label: String, val signatureKind: PluginSignatureMaterial? = null) {
    ARCHIVE("ZIP 发布包"), DESCRIPTOR("发布描述 release.json", PluginSignatureMaterial.DESCRIPTOR),
    SIGNATURE("签名 release.sig", PluginSignatureMaterial.SIGNATURE), POLICY("渠道公钥策略", PluginSignatureMaterial.POLICY)
}

/** Owns exactly one captured archive. All mutations run on the UI dispatcher;
 * captured fd leases and the inspector own the asynchronous IO lifetime. */
internal class PluginArchiveReviewModel(
    private val scope: CoroutineScope,
    private val capture: suspend (Uri) -> PluginArchiveCapture,
    private val read: suspend (Uri, PluginSignatureMaterial) -> PluginSignatureDocument,
    private val inspect: suspend (PluginArchiveCapture, ByteArray, ByteArray, ByteArray?) -> ExtensionController.SignedArchiveReview,
    private val install: suspend (PluginArchiveCapture, ByteArray, ByteArray, ByteArray?, ExtensionController.SignedArchiveReview) -> ExtensionController.InstalledRelease =
        { _, _, _, _, _ -> error("安装服务未接入") },
) {
    class Pick internal constructor(val kind: PluginArchiveMaterial)
    private var closed = false
    private var epoch = Any()
    private var operation: Job? = null
    var pending: Pick? by mutableStateOf(null); private set
    var archive: PluginArchiveCapture? by mutableStateOf(null); private set
    var documents: Map<PluginSignatureMaterial, PluginSignatureDocument> by mutableStateOf(emptyMap()); private set
    var useConfiguredChannel by mutableStateOf(false); private set
    var trustedPolicy by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var receipt: ExtensionController.SignedArchiveReview? by mutableStateOf(null); private set
    var issue: String? by mutableStateOf(null); private set
    var installAcknowledged by mutableStateOf(false); private set
    var installing by mutableStateOf(false); private set
    var installation: ExtensionController.InstalledRelease? by mutableStateOf(null); private set
    val canInspect get() = !closed && !busy && pending == null && archive != null && PluginSignatureMaterial.DESCRIPTOR in documents &&
        PluginSignatureMaterial.SIGNATURE in documents && (useConfiguredChannel || trustedPolicy && PluginSignatureMaterial.POLICY in documents)
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    val canInstall get() = canInspect && receipt != null && installAcknowledged && installation == null
    private fun invalidate() {
        epoch = Any(); receipt = null; issue = null
        installAcknowledged = false; installation = null
    }
    fun beginPick(kind: PluginArchiveMaterial): Pick? {
        main()
        if (closed || busy || pending != null) return null
        invalidate()
        if (kind == PluginArchiveMaterial.ARCHIVE) { archive?.close(); archive = null }
        else documents = documents - kind.signatureKind!!
        if (kind == PluginArchiveMaterial.POLICY) trustedPolicy = false
        return Pick(kind).also { pending = it }
    }
    fun failPick(request: Pick) {
        main()
        if (!closed && pending === request) { pending = null; issue = "无法打开系统文件选择器。" }
    }
    fun completePick(request: Pick, uri: Uri?) {
        main()
        if (closed || pending !== request) return
        pending = null
        if (uri == null) return
        busy = true
        operation = scope.launch {
            try {
                if (request.kind == PluginArchiveMaterial.ARCHIVE) {
                    val value = capture(uri)
                    if (!closed) archive = value else value.close()
                } else {
                    val kind = request.kind.signatureKind!!
                    val value = read(uri, kind)
                    if (!closed) documents = documents + (kind to value)
                }
            } catch (_: TimeoutCancellationException) {
                if (!closed) issue = "读取所选材料超时，请重新选择。"
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (!closed) issue = message(failure) }
            finally { busy = false }
        }
    }
    fun selectConfiguredChannel(value: Boolean) {
        main()
        if (closed || busy || pending != null || value == useConfiguredChannel) return
        invalidate()
        useConfiguredChannel = value
    }
    private fun policyBytes() = if (useConfiguredChannel) null
        else documents.getValue(PluginSignatureMaterial.POLICY).copyBytes()

    fun trustPolicy(value: Boolean) {
        main()
        if (installing) return
        if (closed || value && PluginSignatureMaterial.POLICY !in documents) return
        invalidate(); trustedPolicy = value
    }
    fun acknowledgeInstall(value: Boolean) {
        main()
        if (closed || busy || receipt == null || (!useConfiguredChannel && !trustedPolicy) || installation != null) return
        installAcknowledged = value
    }
    fun installArchive() {
        main()
        if (!canInstall) return
        val generation = epoch
        val selected = archive!!
        val expected = receipt!!
        val descriptor = documents.getValue(PluginSignatureMaterial.DESCRIPTOR).copyBytes()
        val signature = documents.getValue(PluginSignatureMaterial.SIGNATURE).copyBytes()
        val policy = policyBytes()
        busy = true; installing = true; issue = null
        operation = scope.launch {
            try {
                val value = install(selected, descriptor, signature, policy, expected)
                if (!closed && generation === epoch) installation = value
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) {
                if (!closed && generation === epoch) issue = when (failure) {
                    is PluginReleaseStage.Rejected, is PluginReleaseSignature.Rejected, is PluginReleaseArchive.Rejected -> failure.message
                    else -> "未取得完整安装回执；请核对是否有版本、暂存或归属记录保留。"
                }
            } finally { installAcknowledged = false; busy = false; installing = false }
        }
    }
    fun checkArchive() {
        main()
        if (!canInspect) return
        invalidate()
        val generation = epoch
        val selected = archive!!
        val descriptor = documents.getValue(PluginSignatureMaterial.DESCRIPTOR).copyBytes()
        val signature = documents.getValue(PluginSignatureMaterial.SIGNATURE).copyBytes()
        val policy = policyBytes()
        busy = true
        operation = scope.launch {
            try {
                val value = inspect(selected, descriptor, signature, policy)
                if (!closed && generation === epoch && (useConfiguredChannel || trustedPolicy)) receipt = value
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) { if (!closed && generation === epoch) issue = message(failure) }
            finally { busy = false }
        }
    }
    fun close() {
        main(); closed = true; invalidate(); pending = null; trustedPolicy = false
        operation?.cancel(); archive?.close(); archive = null; documents = emptyMap()
    }
    private fun message(failure: Throwable) = when (failure) {
        is PluginArchiveCapture.Rejected, is PluginSignatureReadFailure, is PluginReleaseSignature.Rejected,
        is PluginReleaseArchive.Rejected, is PluginReleaseDeclarations.Rejected -> failure.message ?: "材料无效，请重新选择。"
        else -> "无法完成签名 ZIP 预检，请重新选择。"
    }
}

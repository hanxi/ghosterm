package com.hanxi.ghosterm

import android.content.ContentResolver
import android.net.Uri
import android.os.CancellationSignal
import android.os.Looper
import android.os.SystemClock
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.InternalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.security.MessageDigest
import java.util.concurrent.Semaphore

internal enum class PluginSignatureMaterial(val label: String, val limit: Int) {
    DESCRIPTOR("发布描述 release.json", 65536), SIGNATURE("签名 release.sig", 64), POLICY("渠道公钥策略", 131072)
}

/** Private captured bytes. Public state exposes only size and receipt digest. */
internal class PluginSignatureDocument(bytes: ByteArray) {
    private val content = bytes.copyOf()
    val size = content.size
    val sha256 = MessageDigest.getInstance("SHA-256").digest(content).joinToString("") { "%02x".format(it.toInt() and 255) }
    fun copyBytes() = content.copyOf()
}

internal class PluginSignatureReadFailure(message: String) : IllegalArgumentException(message)

/** Own the content fd, ignore advertised lengths, capture to actual EOF. */
internal object PluginSignatureDocuments {
    private val readers = Semaphore(2)
    // ZIP streaming shares the same process-wide admission bound. A cancelled
    // provider keeps its slot until the actual IO worker has exited.
    internal fun acquireArchiveSlot() = readers.tryAcquire()
    internal fun releaseArchiveSlot() = readers.release()
    private inline fun demand(value: Boolean, message: () -> String) {
        if (!value) throw PluginSignatureReadFailure(message())
    }
    @OptIn(InternalCoroutinesApi::class)
    suspend fun read(resolver: ContentResolver, uri: Uri, kind: PluginSignatureMaterial): PluginSignatureDocument = withTimeout(10000) { withContext(Dispatchers.IO) {
        demand(uri.scheme == "content" && !uri.authority.isNullOrBlank()) { "请选择文档提供方中的公开签名材料。" }
        demand(readers.tryAcquire()) { "先前的材料读取尚未释放，请稍后再试。" }
        try {
        val context = currentCoroutineContext()
        val cancellation = CancellationSignal()
        // CancellationSignal also reaches a cooperative provider during open;
        // stream reads below use short poll intervals and nonblocking mode.
        val registration = context[Job]!!.invokeOnCompletion(onCancelling = true, invokeImmediately = true) { cancellation.cancel() }
        try {
            val started = SystemClock.elapsedRealtime()
            context.ensureActive()
            (resolver.openFileDescriptor(uri, "r", cancellation) ?: error("无法打开所选文件。")).use { owner ->
                context.ensureActive()
                val fd = owner.fileDescriptor
                val before = Os.fstat(fd)
                val regular = OsConstants.S_ISREG(before.st_mode)
                demand(regular || OsConstants.S_ISFIFO(before.st_mode)) { "所选材料不是普通文件或可读取的数据流。" }
                val flags = FileDescriptorFlags.status(fd)
                demand(flags and OsConstants.O_ACCMODE == OsConstants.O_RDONLY) { "所选材料没有提供只读文件句柄。" }
                demand(FileDescriptorFlags.descriptorFlags(owner.fd) and OsConstants.FD_CLOEXEC != 0) { "所选材料没有提供可安全读取的文件句柄。" }
                FileDescriptorFlags.status(fd, flags or OsConstants.O_NONBLOCK)
                try {
                val bytes = ByteArray(kind.limit + 1)
                var count = 0
                val poll = StructPollfd().apply { this.fd = fd; events = OsConstants.POLLIN.toShort() }
                while (true) {
                    context.ensureActive()
                    demand(SystemClock.elapsedRealtime() - started < 10000) { "读取所选材料超时，请重新选择。" }
                    if (Os.poll(arrayOf(poll), 100) == 0) continue
                    demand(poll.revents.toInt() and (OsConstants.POLLERR or OsConstants.POLLNVAL) == 0) { "所选材料的数据流已失效。" }
                    val read = try { Os.read(fd, bytes, count, bytes.size - count) }
                    catch (failure: ErrnoException) {
                        if (failure.errno == OsConstants.EINTR || failure.errno == OsConstants.EAGAIN) continue
                        throw failure
                    }
                    if (read == 0) break
                    count += read
                    demand(count <= kind.limit) { "${kind.label}超过 ${kind.limit} 字节上限。" }
                }
                owner.checkError()
                val after = Os.fstat(fd)
                demand(!regular || before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                    before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                    before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
                    before.st_ctim.tv_nsec == after.st_ctim.tv_nsec) {
                    "所选文件在读取期间发生变化，请重新选择。"
                }
                demand(count > 0 && (kind != PluginSignatureMaterial.SIGNATURE || count == 64)) { "签名材料为空或签名长度不是 64 字节。" }
                context.ensureActive()
                PluginSignatureDocument(bytes.copyOf(count))
                } finally { FileDescriptorFlags.status(fd, flags) }
            }
        } catch (failure: Exception) { context.ensureActive(); throw failure }
        finally { registration.dispose() }
        } finally { readers.release() }
    } }
}

/** Page-local draft; tokens bind asynchronous picker/read/verify completions. */
internal class PluginSignatureReviewModel(
    private val scope: CoroutineScope,
    private val read: suspend (Uri, PluginSignatureMaterial) -> PluginSignatureDocument,
    private val verify: suspend (ByteArray, ByteArray, ByteArray) -> PluginReleaseSignature.Proof,
) {
    class Pick internal constructor(val kind: PluginSignatureMaterial)
    private var epoch = Any()
    private var closed = false
    private var operation: Job? = null
    var pending: Pick? by mutableStateOf(null); private set
    var documents: Map<PluginSignatureMaterial, PluginSignatureDocument> by mutableStateOf(emptyMap()); private set
    var trustedPolicy by mutableStateOf(false); private set
    var busy by mutableStateOf(false); private set
    var receipt: PluginReleaseSignature.Proof? by mutableStateOf(null); private set
    var issue: String? by mutableStateOf(null); private set
    val canVerify get() = !closed && !busy && pending == null && trustedPolicy && documents.size == 3
    private fun main() { check(Looper.myLooper() == Looper.getMainLooper()) }
    private fun invalidate() { epoch = Any(); receipt = null; issue = null }

    fun beginPick(kind: PluginSignatureMaterial): Pick? {
        main()
        if (closed || busy || pending != null) return null
        invalidate()
        documents = documents - kind
        if (kind == PluginSignatureMaterial.POLICY) trustedPolicy = false
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
        val generation = epoch
        busy = true
        operation = scope.launch {
            try {
                val value = read(uri, request.kind)
                if (!closed && generation === epoch) documents = documents + (request.kind to value)
            } catch (_: TimeoutCancellationException) {
                if (!closed && generation === epoch) issue = "读取所选材料超时，请重新选择。"
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) {
                if (!closed && generation === epoch) issue = when (failure) {
                    is PluginSignatureReadFailure -> failure.message ?: "所选材料无效。"
                    else -> "无法完整读取所选材料，请重新选择。"
                }
            } finally { busy = false }
        }
    }
    fun trustPolicy(value: Boolean) {
        main()
        if (closed || value && PluginSignatureMaterial.POLICY !in documents) return
        invalidate(); trustedPolicy = value
    }
    fun checkSignature() {
        main()
        if (!canVerify) return
        invalidate()
        val generation = epoch
        val descriptor = documents.getValue(PluginSignatureMaterial.DESCRIPTOR).copyBytes()
        val signature = documents.getValue(PluginSignatureMaterial.SIGNATURE).copyBytes()
        val policy = documents.getValue(PluginSignatureMaterial.POLICY).copyBytes()
        busy = true
        operation = scope.launch {
            try {
                val proof = verify(descriptor, signature, policy)
                if (!closed && generation === epoch && trustedPolicy) receipt = proof
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (failure: Throwable) {
                if (!closed && generation === epoch) issue = if (failure is PluginReleaseSignature.Rejected)
                    failure.message else "无法核对当前固定包，请重新审查包与公开材料。"
            } finally { busy = false }
        }
    }
    fun close() {
        main(); closed = true; invalidate(); pending = null; trustedPolicy = false
        documents = emptyMap(); operation?.cancel()
    }
}

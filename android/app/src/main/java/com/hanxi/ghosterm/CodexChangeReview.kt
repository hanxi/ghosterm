package com.hanxi.ghosterm

import java.io.ByteArrayOutputStream
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

internal fun codexChangeUtf8(value: String): ByteArray {
    val encoded = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
    return ByteArray(encoded.remaining()).also { encoded.get(it) }
}

internal data class CodexChangeReference(val snapshotID: String, val threadID: String, val turnID: String,
    val itemID: String, val totalUnits: Int, val totalBytes: Int) {
    companion object {
        const val MAX_BYTES = 524288
        fun create(id: String, thread: String, turn: String, item: String, units: Long, bytes: Long): CodexChangeReference? {
            if (!id.matches(Regex("change-[1-9][0-9]{0,15}")) || (id.removePrefix("change-").toLongOrNull() ?: Long.MAX_VALUE) > 9007199254740991L ||
                units <= 0 || bytes < units || bytes > MAX_BYTES ||
                listOf(thread, turn, item).any { it.isEmpty() || codexChangeUtf8(it).size > 128 }) return null
            return CodexChangeReference(id, thread, turn, item, units.toInt(), bytes.toInt())
        }
    }
}

internal data class CodexCompleteChangeFile(val path: String, val kind: String, val movePath: String?, val diff: String)
internal data class CodexChangeDocument(val threadID: String, val turnID: String, val itemID: String,
    val files: List<CodexCompleteChangeFile>)
internal data class CodexChangeChunk(val status: String, val snapshotID: String, val offset: Int, val nextOffset: Int,
    val totalUnits: Int, val totalBytes: Int, val content: String, val done: Boolean)
internal data class CodexChangeReviewState(val reference: CodexChangeReference, val receivedBytes: Int = 0,
    val document: CodexChangeDocument? = null, val issue: String? = null) {
    val loading get() = document == null && issue == null
}

/** Pure Kotlin assembly; no partial content is published and no text is normalized. */
internal class CodexChangeAssembler(val reference: CodexChangeReference) {
    private val bytes = ByteArrayOutputStream()
    var nextOffset = 0
        private set
    val receivedBytes get() = bytes.size()
    var done = false
        private set

    fun append(chunk: CodexChangeChunk) {
        check(!done) { "完整补丁已结束，不能继续追加。" }
        check(chunk.status != "expired") { "补丁快照已过期，请读取当前版本。" }
        require(chunk.status == "ok" && chunk.snapshotID == reference.snapshotID &&
            chunk.totalUnits == reference.totalUnits && chunk.totalBytes == reference.totalBytes &&
            chunk.offset == nextOffset && chunk.content.length <= reference.totalUnits - nextOffset &&
            chunk.nextOffset == nextOffset + chunk.content.length &&
            chunk.done == (chunk.nextOffset == reference.totalUnits) &&
            (chunk.done || chunk.nextOffset > nextOffset)) { "完整补丁分块或身份无效。" }
        val data = codexChangeUtf8(chunk.content)
        require(data.size <= 3072 && data.size <= reference.totalBytes - receivedBytes) { "完整补丁字节数超限。" }
        require(!chunk.done || receivedBytes + data.size == reference.totalBytes) { "完整补丁实际字节数不匹配。" }
        bytes.write(data)
        nextOffset = chunk.nextOffset
        done = chunk.done
    }

    fun finishBytes(): ByteArray {
        check(done && receivedBytes == reference.totalBytes && nextOffset == reference.totalUnits) { "完整补丁尚未读完。" }
        return bytes.toByteArray()
    }
}

/** Caller supplies the live Session/reference check; cancellation never publishes old data. */
internal class CodexChangeReviewController(private val scope: CoroutineScope,
    private val decodeDispatcher: CoroutineDispatcher = Dispatchers.Default,
    private val timeoutMillis: Long = 120000) {
    init { require(timeoutMillis in 1..120000) }
    private val mutable = MutableStateFlow<CodexChangeReviewState?>(null)
    val state = mutable.asStateFlow()
    private var generation = Any()
    private var task: Job? = null

    fun cancel() {
        generation = Any()
        task?.cancel()
        task = null
        mutable.value = null
    }

    fun invalidate(isCurrent: (CodexChangeReference) -> Boolean) {
        state.value?.let { if (!isCurrent(it.reference)) cancel() }
    }

    fun load(reference: CodexChangeReference, isCurrent: () -> Boolean,
        read: suspend (Int) -> CodexChangeChunk,
        decode: (ByteArray, CodexChangeReference) -> CodexChangeDocument): Job {
        cancel()
        val token = generation
        mutable.value = CodexChangeReviewState(reference)
        return scope.launch {
            val assembler = CodexChangeAssembler(reference)
            try {
                withTimeout(timeoutMillis) {
                    repeat(2048) {
                        currentCoroutineContext().ensureActive()
                        if (generation !== token) return@withTimeout
                        if (!isCurrent()) { cancel(); return@withTimeout }
                        val chunk = read(assembler.nextOffset)
                        currentCoroutineContext().ensureActive()
                        if (generation !== token) return@withTimeout
                        if (!isCurrent()) { cancel(); return@withTimeout }
                        assembler.append(chunk)
                        mutable.value = CodexChangeReviewState(reference, assembler.receivedBytes)
                        if (assembler.done) {
                            val complete = assembler.finishBytes()
                            val document = withContext(decodeDispatcher) { decode(complete, reference) }
                            currentCoroutineContext().ensureActive()
                            if (generation !== token) return@withTimeout
                            if (!isCurrent()) { cancel(); return@withTimeout }
                            mutable.value = CodexChangeReviewState(reference, assembler.receivedBytes, document)
                            return@withTimeout
                        }
                    }
                    error("完整补丁分块数量超过上限。")
                }
            } catch (timeout: TimeoutCancellationException) {
                if (generation === token) {
                    if (!isCurrent()) cancel()
                    else mutable.value = CodexChangeReviewState(reference, assembler.receivedBytes,
                        issue = "完整补丁读取超时，未完成的内容不会显示。")
                }
            } catch (cancelled: CancellationException) {
                if (generation === token) mutable.value = null
                throw cancelled
            } catch (failure: Exception) {
                if (generation === token) {
                    if (!isCurrent()) cancel()
                    else mutable.value = CodexChangeReviewState(reference, assembler.receivedBytes,
                        issue = failure.message ?: "完整补丁读取失败。")
                }
            } finally {
                if (generation === token) task = null
            }
        }.also { task = it }
    }
}

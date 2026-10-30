package com.hanxi.ghosterm.bridge

import android.os.ParcelFileDescriptor
import kotlinx.coroutines.NonCancellable
import java.nio.CharBuffer
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

/** File control data only; never terminal output or native handles. */
data class RemoteFileEntry(val path: String, val directory: Boolean, val symbolicLink: Boolean,
    val regularFile: Boolean, val size: ULong?, val modifiedSeconds: Long?) {
    val name: String get() = path.substringAfterLast('/')
}
data class RemoteDirectory(val path: String, val entries: List<RemoteFileEntry>)
data class RemoteFileState(val phase: Int, val error: Int) {
    val ready get() = phase == 1
    val finished get() = phase >= 2
}
enum class RemoteCreationState { NOT_ATTEMPTED, UNKNOWN, REJECTED, CREATED, COMPLETE }
data class RemoteCreationResult(val status: Int, val state: RemoteCreationState, val bytesAcknowledged: ULong)

class RemoteFileException(val status: Int) : IllegalStateException(when(status) {
    7 -> "此文件类型或 SFTP 操作暂不支持"
    8 -> "文件请求已取消"
    else -> "文件请求失败或超出读取限制（错误 $status），请重新连接"
})

/** A dedicated authenticated SSH/SFTP connection. A cancelled operation closes
 * this owner; retry creates a new connection. close() only signals native work. */
class RemoteFileSession(connection: ResolvedSSHConnection) : AuthenticatedSSH {
    private var handle = connection.openFiles()
    private val requests = Mutex()
    @Synchronized override fun authentication(): SSHAuthState {
        check(handle != 0L) { "文件连接已关闭" }
        return SSHAuthState.decode(handle, NativeFiles.authentication(handle), this)
    }
    @Synchronized fun state(): RemoteFileState {
        if(handle == 0L) return RemoteFileState(3, 8)
        val fields = NativeFiles.state(handle)
        check(fields.size == 2 && fields[0] in 0..3) { "文件状态格式无效" }
        return RemoteFileState(fields[0], fields[1])
    }
    override fun decide(prompt: SSHAuthState, accept: Boolean) = respond(prompt, if(accept) 1 else 2)
    override fun password(prompt: SSHAuthState, password: ByteArray) = respond(prompt, 3, password)
    override fun privateKey(prompt: SSHAuthState, pem: ByteArray, passphrase: ByteArray) = respond(prompt, 4, pem, passphrase)
    @Synchronized private fun respond(prompt: SSHAuthState, kind: Int, first: ByteArray = byteArrayOf(), second: ByteArray = byteArrayOf()) {
        try {
            check(handle != 0L && prompt.owner == handle && prompt.scope === this) { "SSH 请求不属于此文件连接" }
            NativeFiles.respond(handle, prompt.request, kind, first, second)
        } finally { first.fill(0); second.fill(0) }
    }
    @Synchronized override fun close() {
        NativeFiles.dispose(handle); handle = 0L
    }
    private suspend fun operation(kind: Int, path: String, timeoutMillis: Int, sourceFd: Int = -1, expectedSize: Long = 0): ByteArray = withContext(Dispatchers.IO) {
        require(path.isNotEmpty() && '\u0000' !in path && timeoutMillis in 1..60_000) { "文件路径或超时无效" }
        requests.withLock {
            // Native duplicates the write end and closes it only AFTER its data
            // fd. EOF survives dispose/recycled handles without a Java callback.
            val released = if(kind==3 || kind==4) ParcelFileDescriptor.createPipe() else null
            try {
            val (owner, request) = synchronized(this@RemoteFileSession) {
                check(handle != 0L) { "文件连接已关闭" }
                val encoded = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(path))
                val bytes = ByteArray(encoded.remaining()); encoded.get(bytes)
                handle to if(kind==3) NativeFiles.beginCreate(handle,bytes,sourceFd,expectedSize,timeoutMillis,checkNotNull(released)[1].fd)
                    else if(kind==4) NativeFiles.beginReceive(handle,bytes,sourceFd,expectedSize,timeoutMillis,checkNotNull(released)[1].fd)
                    else NativeFiles.begin(handle, kind, bytes, timeoutMillis)
            }
            var completed = false
            try {
                released?.get(1)?.close()
                var response: ByteArray? = null
                while(response == null) {
                    response = synchronized(this@RemoteFileSession) {
                        check(handle == owner) { "文件连接已关闭" }
                        NativeFiles.result(owner, request)
                    }
                    if(response == null) delay(10)
                }
                val bytes = checkNotNull(response)
                check(bytes.size >= 8) { "文件响应格式无效" }
                val header = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
                val status = header.int
                check(header.int == kind) { "文件响应归属无效" }
                synchronized(this@RemoteFileSession) {
                    check(handle == owner) { "文件连接已关闭" }
                    NativeFiles.finish(owner, request)
                }
                if(status != 0 && kind!=3) throw RemoteFileException(status)
                completed = true
                if(kind==3) {
                    if(status!=0) close()
                    bytes
                } else bytes.copyOfRange(8, bytes.size)
            } finally {
                if(!completed) close()
            }
            } finally {
                if(released != null) {
                    released[1].close()
                    // Keep the caller's staging reservation until the duplicate
                    // actually closes, including cancellation before execution.
                    withContext(NonCancellable + Dispatchers.IO) {
                        ParcelFileDescriptor.AutoCloseInputStream(released[0]).use {
                            check(it.read()==-1) { "文件释放通知格式无效" }
                        }
                    }
                }
            }
        }
    }
    suspend fun browse(path: String = ".", timeoutMillis: Int = 5000): RemoteDirectory = withContext(Dispatchers.IO) {
        val data = ByteBuffer.wrap(operation(1,path,timeoutMillis)).order(ByteOrder.BIG_ENDIAN)
        fun string(): String {
            check(data.remaining() >= 4) { "目录数据缺少长度" }
            val length = data.int
            check(length in 1..65535 && length <= data.remaining()) { "目录路径长度无效" }
            val bytes = ByteArray(length); data.get(bytes)
            check(bytes.none { it == 0.toByte() }) { "远端路径包含 NUL" }
            return utf8(bytes)
        }
        val canonical = string()
        check(canonical.startsWith('/')) { "远端目录不是绝对路径" }
        check(data.remaining() >= 4) { "目录数据不完整" }
        val count = data.int
        check(count in 0..4096) { "目录条目超限" }
        val prefix = canonical.trimEnd('/') + "/"
        val entries = List(count) {
            val full = string()
            check(full.startsWith(prefix) && full.removePrefix(prefix).let { it.isNotEmpty() && '/' !in it && it != "." && it != ".." }) { "目录条目不属于当前路径" }
            check(data.remaining() >= 20) { "文件属性不完整" }
            val flags = data.int
            check(flags in 0..15) { "文件属性标志无效" }
            val size = data.long.toULong(); val modified = data.long
            RemoteFileEntry(full, flags and 1 != 0, flags and 2 != 0, flags and 4 != 0,
                size.takeIf { flags and 8 != 0 }, modified.takeIf { it >= 0 })
        }
        check(!data.hasRemaining() && entries.map { it.path }.toSet().size == entries.size) { "目录结果重复或包含额外数据" }
        RemoteDirectory(canonical, entries)
    }
    /** Create a task-owned staging file, never overwrite. Keep sourceFd open and
     * content stable until return; JNI owns a CLOEXEC duplicate during IO.
     * An exception/coroutine cancellation without a result means UNKNOWN: do
     * not automatically retry or unlink. Final destination commit is separate. */
    suspend fun createExclusive(path: String, sourceFd: Int, expectedSize: Long,
                                timeoutMillis: Int = 5000): RemoteCreationResult {
        require(sourceFd>=0 && expectedSize>=0) { "文件源描述符或尺寸无效" }
        val data=ByteBuffer.wrap(operation(3,path,timeoutMillis,sourceFd,expectedSize)).order(ByteOrder.BIG_ENDIAN)
        check(data.remaining()==20) { "创建结果无法确认，请保留目标路径" }
        val status=data.int
        check(data.int==3) { "创建结果归属无效" }
        val state=data.int
        check(state in 0..4) { "创建状态无法确认" }
        val count=data.long.toULong()
        check((status==0)==(state==4) && count<=expectedSize.toULong()) { "创建结果不一致" }
        return RemoteCreationResult(status,RemoteCreationState.entries[state],count)
    }
    /** Receive into an empty task-owned regular fd, never a final destination.
     * On error discard partial bytes. Return/cancellation waits for native fd
     * release, so a surrounding staging lease remains valid throughout IO. */
    suspend fun receive(path: String, destinationFd: Int, maxBytes: Long,
                        timeoutMillis: Int = 5000): Long {
        require(destinationFd>=0 && maxBytes>=0) { "文件目标描述符或限额无效" }
        val data=ByteBuffer.wrap(operation(4,path,timeoutMillis,destinationFd,maxBytes)).order(ByteOrder.BIG_ENDIAN)
        check(data.remaining()==8) { "接收结果格式无效" }
        val count=data.long
        check(count in 0..maxBytes) { "接收结果大小无效" }
        return count
    }
    suspend fun preview(path: String, timeoutMillis: Int = 5000): ByteArray = operation(2,path,timeoutMillis)
    suspend fun previewText(path: String, timeoutMillis: Int = 5000): String = withContext(Dispatchers.IO) {
        val bytes = preview(path,timeoutMillis)
        try {
            check(bytes.none { it == 0.toByte() }) { "此文件不是可预览的 UTF-8 文本" }
            utf8(bytes)
        } finally { bytes.fill(0) }
    }
    private fun utf8(bytes: ByteArray): String = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes)).toString()
}

internal object NativeFiles {
    init { System.loadLibrary("ghosterm_android") }
    external fun create(host: ByteArray, port: Int, user: ByteArray, known: ByteArray, addresses: Array<ByteArray>, scopes: IntArray, timeout: Int): Long
    external fun authentication(handle: Long): Array<ByteArray>
    external fun respond(handle: Long, request: Long, kind: Int, first: ByteArray, second: ByteArray)
    external fun state(handle: Long): IntArray
    external fun dispose(handle: Long)
    external fun begin(handle: Long, kind: Int, path: ByteArray, budget: Int): Long
    external fun beginCreate(handle: Long, path: ByteArray, sourceFd: Int, expectedSize: Long, budget: Int, releaseFd: Int): Long
    external fun beginReceive(handle: Long, path: ByteArray, destinationFd: Int, maxBytes: Long, budget: Int, releaseFd: Int): Long
    external fun result(handle: Long, request: Long): ByteArray?
    external fun finish(handle: Long, request: Long)
    external fun ownerCount(): Int
}

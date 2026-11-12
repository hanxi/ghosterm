package com.hanxi.ghosterm

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.database.Cursor
import com.hanxi.ghosterm.bridge.ExtensionPackage
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.SSHAuthState
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.android.asCoroutineDispatcher
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.Inet6Address
import java.net.InetAddress
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

/** App-side Client package owner. Native package and registry calls stay on one
 * non-UI worker. Reviewing never grants source trust or activates JavaScript. */
class ExtensionController(profile: HostProfile, context: Context) {
    enum class Source { UNKNOWN, DENIED, ALLOWED }

    data class HostProfile(
        val sdkVersion: String,
        val protocols: List<Int>,
        val runtimes: List<String>,
        val capabilities: List<String>,
    ) {
        init {
            require(sdkVersion.isNotBlank() && '\u0000' !in sdkVersion && sdkVersion.toByteArray().size <= 128)
            require(protocols.size <= 8 && protocols.all { it in 1..65535 } && protocols.distinct().size == protocols.size)
            require(runtimes.size <= 16 && runtimes.all(::validName) && runtimes.distinct().size == runtimes.size)
            require(capabilities.size <= 16 && capabilities.all(::validName) && capabilities.distinct().size == capabilities.size)
        }

        internal fun json(): String = JSONObject().apply {
            put("side", "client")
            put("sdkVersion", sdkVersion)
            put("protocols", JSONArray(protocols))
            put("runtimes", JSONArray(runtimes))
            put("capabilities", JSONArray(capabilities))
        }.toString().also { require(it.toByteArray().size <= 65536) }
    }

    data class Command(val path: String, val title: String, val argsSchema: String, val views: List<String>)
    data class Review(
        val id: String, val version: String, val publisher: String, val license: String,
        val digest: String, val runtime: String, val requiredCapabilities: List<String>,
        val optionalMissingCapabilities: List<String>, val authorizationGranted: Boolean,
        val publisherAuthenticated: Boolean,
        val commands: List<Command>, val sourceApproved: Boolean = false,
    )
    data class Resolution(
        val status: ExtensionRegistry.Status,
        val packageID: String? = null,
        val packageDigest: String? = null,
        val command: Command? = null,
    )
    data class ChannelState(val info: ExtensionChannel.Info, val authentication: SSHAuthState)
    data class SessionState(val packageID: String, val packageDigest: String, val info: ExtensionSession.Info)
    data class RuntimeState(val packageID: String, val packageDigest: String, val command: String, val info: ExtensionRuntime.Info)

    private data class Key(val id: String, val digest: String)
    private data class Record(val packageBytes: ExtensionPackage, val review: Review, var source: Source)

    private val thread = HandlerThread("ghosterm-extension-controller", Thread.NORM_PRIORITY).apply { start() }
    private val dispatcher: CoroutineDispatcher = Handler(thread.looper).asCoroutineDispatcher("ghosterm-extension-controller")
    private val resolver: ContentResolver = context.applicationContext.contentResolver
    private val stagingRoot = File(context.applicationContext.cacheDir, "extension-package-review")
    private val hostJSON = profile.json()
    private val hostCapabilities = profile.capabilities.toList()
    private val closed = AtomicBoolean(false)
    private val cleanupComplete = AtomicBoolean(false)
    private val records = linkedMapOf<Key, Record>() // worker-confined
    private var registry: ExtensionRegistry? = null // worker-confined
    @Volatile private var channel: ExtensionChannel? = null // stop alone may cross threads
    @Volatile private var session: ExtensionSession? = null
    private var sessionPackageKey: Key? = null
    @Volatile private var runtime: ExtensionRuntime? = null
    private var runtimePackageKey: Key? = null
    private var runtimeCommand: String? = null

    /** Create an explicitly requested gterm relay route. This does not install,
     * discover, or start a remote Executor; the caller supplies its exact command. */
    suspend fun openSSHChannel(hops: List<ExtensionChannel.Hop>, family: Int, address: ByteArray,
        scope: Int = 0, bootstrap: String): ChannelState = onWorker {
        check(channel == null) { "已有插件通道占用此控制器" }
        val opened = ExtensionChannel.ssh(hops, family, address.copyOf(), scope, bootstrap)
        channel = opened
        try {
            opened.step(0)
            ChannelState(opened.info(), opened.authentication())
        } catch (failure: Throwable) {
            opened.stop()
            throw failure
        }
    }

    /** Consume the same resolved OpenSSH route as terminal/SFTP connections.
     * DNS is already resolved by the caller; only hop zero receives an address
     * and every inner connector targets that hop's configured HostName. */
    suspend fun openSSHChannel(target: PreparedSSH, address: InetAddress, knownHosts: File,
        bootstrap: String): ChannelState {
        require(knownHosts.isAbsolute && '\u0000' !in knownHosts.path && knownHosts.path.toByteArray().size <= 4096)
        val route = target.jumps + target
        require(route.size in 1..8 && route.all { it.user.isNotBlank() })
        val hops = route.mapIndexed { index, hop ->
            ExtensionChannel.Hop(hop.host, hop.port, hop.user, knownHosts.path,
                if (index == 0) null else hop.host)
        }
        val raw = address.address
        require(raw.size == 4 || raw.size == 16)
        val family = if (raw.size == 4) 4 else 6
        val packed = ByteArray(16).also { raw.copyInto(it) }
        val scope = (address as? Inet6Address)?.scopeId ?: 0
        return openSSHChannel(hops, family, packed, scope, bootstrap)
    }

    /** Poll and expose authentication state without ever invoking the
     * thread-affine channel from the UI coroutine's thread. */
    suspend fun stepSSHChannel(milliseconds: Int = 0): ChannelState = onWorker {
        val current = checkNotNull(channel) { "插件通道尚未打开" }
        current.step(milliseconds)
        ChannelState(current.info(), current.authentication())
    }

    suspend fun decideSSHHost(prompt: SSHAuthState, accept: Boolean) = onWorker {
        checkNotNull(channel) { "插件通道尚未打开" }.decide(prompt, accept)
    }

    suspend fun submitSSHPassword(prompt: SSHAuthState, password: ByteArray) {
        try { onWorker { checkNotNull(channel) { "插件通道尚未打开" }.password(prompt, password) } }
        finally { password.fill(0) }
    }

    suspend fun submitSSHPrivateKey(prompt: SSHAuthState, pem: ByteArray, passphrase: ByteArray) {
        try { onWorker { checkNotNull(channel) { "插件通道尚未打开" }.privateKey(prompt, pem, passphrase) } }
        finally { pem.fill(0); passphrase.fill(0) }
    }

    suspend fun stopSSHChannel() {
        onWorker { channel?.stop() }
    }

    /** Cancellation wakeup only; native stop is explicitly cross-thread safe. */
    fun requestStopSSHChannel() { channel?.stop() }

    suspend fun closeSSHChannel() = onWorker {
        channel?.let { current -> current.close(); channel = null }
    }

    /** Transfer a ready SSH relay to a HOST Session using only the already
     * approved fixed Client package and caller-selected Executor identity. */
    suspend fun adoptSSHSession(pluginID: String, packageDigest: String, executorDigest: String,
        hostVersion: String, peerIdentity: String, methods: List<ExtensionSession.Method>): SessionState = onWorker {
        check(session == null && runtime == null) { "此 controller 已有活动插件 Session" }
        val key = Key(pluginID, packageDigest)
        val record = records[key] ?: error("插件包摘要已失效")
        check(record.source == Source.ALLOWED) { "插件来源尚未获本进程批准" }
        require(executorDigest.matches(Regex("[a-f0-9]{64}"))) { "Executor 摘要无效" }
        require(hostVersion.isNotBlank() && hostVersion.toByteArray().size <= 64 && '\u0000' !in hostVersion)
        require(peerIdentity.isNotBlank() && peerIdentity.toByteArray().size <= 255 && '\u0000' !in peerIdentity)
        val bytes = checkNotNull(channel) { "插件 SSH 通道尚未打开" }
        check(bytes.info().phase == 1) { "插件 SSH 通道尚未就绪" }
        val metadata = record.packageBytes.metadata()
        check(metadata.id == pluginID && metadata.packageDigest == packageDigest && !metadata.executor) {
            "固定包不是所选 Client 包"
        }
        val owned = ExtensionSession.adopt(bytes, ExtensionSession.Role.HOST,
            ExtensionSession.Config(metadata.id, metadata.version, metadata.packageDigest, executorDigest,
                hostVersion, metadata.sdkVersion, metadata.requires, hostCapabilities,
                ExtensionSession.PeerKind.SSH, peerIdentity, methods = methods.toList()))
        channel = null; session = owned; sessionPackageKey = key
        SessionState(pluginID, packageDigest, owned.info())
    }

    suspend fun stepSession(milliseconds: Int = 0): SessionState = onWorker {
        val current = checkNotNull(session) { "插件 Session 尚未接管" }
        current.step(milliseconds)
        val key = checkNotNull(sessionPackageKey)
        SessionState(key.id, key.digest, current.info())
    }

    suspend fun decideSession(consentToken: String, decision: ExtensionSession.Decision): SessionState = onWorker {
        val current = checkNotNull(session) { "插件 Session 尚未接管" }
        val info = current.info()
        check(info.phase == ExtensionSession.Phase.CONSENT && info.consentToken == consentToken) {
            "插件授权请求已失效"
        }
        check(current.decide(consentToken, decision) == 0) { "插件授权决定未被接收" }
        val key = checkNotNull(sessionPackageKey)
        SessionState(key.id, key.digest, current.info())
    }

    /** Activate only after native Session consent reaches ACTIVE. The command
     * is resolved against the exact approved package snapshot before runtime
     * adopts the Session. */
    suspend fun activateClient(pluginID: String, packageDigest: String, command: String,
        arguments: List<String>): RuntimeState = onWorker {
        val current = checkNotNull(session) { "插件 Session 尚未接管" }
        check(current.info().phase == ExtensionSession.Phase.ACTIVE) { "插件 Session 尚未获得授权" }
        check(sessionPackageKey == Key(pluginID, packageDigest)) { "插件 Session 与所选包不匹配" }
        val record = records[Key(pluginID, packageDigest)] ?: error("插件包摘要已失效")
        check(record.source == Source.ALLOWED && record.review.commands.any { it.path == command }) {
            "命令不属于已批准的固定插件包"
        }
        val selected = registryOnWorker().resolve(command, executor = false, selectedId = pluginID,
            selectedDigest = packageDigest)
        val exact = selected.packageBytes ?: error("命令解析失败：${selected.status}")
        try {
            check(selected.command?.path == command && exact.metadata().packageDigest == packageDigest) {
                "命令解析没有返回精确插件包"
            }
        } finally { exact.close() }
        val active = ExtensionRuntime.adoptPackage(current, record.packageBytes, ExtensionSession.Role.HOST,
            associated = true, invocation = ExtensionRuntime.Invocation(command, arguments.toList()))
        session = null; sessionPackageKey = null; runtime = active
        runtimePackageKey = Key(pluginID, packageDigest); runtimeCommand = command
        RuntimeState(pluginID, packageDigest, command, active.info())
    }

    suspend fun stepRuntime(): RuntimeState = onWorker {
        val current = checkNotNull(runtime) { "插件 Runtime 尚未启动" }
        current.step()
        val key = checkNotNull(runtimePackageKey)
        RuntimeState(key.id, key.digest, checkNotNull(runtimeCommand), current.info())
    }

    /** Runtime stop is the only Native runtime operation allowed off-worker. */
    fun requestStopRuntime() { runtime?.stop() }
    /** Session stop is the only Native Session operation allowed off-worker. */
    fun requestStopSession() { session?.stop() }

    /** Release a completed or stopped flow while retaining reviewed packages. */
    suspend fun releaseFinished() = onWorker {
        runtime?.let { current ->
            check(current.info().phase !in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING,
                ExtensionRuntime.Phase.ACTIVE)) { "请先停止仍在运行的插件" }
            current.close()
            runtime = null; runtimePackageKey = null; runtimeCommand = null
        }
        session?.let { current ->
            check(current.info().phase in setOf(ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED)) {
                "不能释放仍活动或等待授权的插件 Session"
            }
            current.close()
            session = null; sessionPackageKey = null
        }
    }

    suspend fun review(directory: File): Review = onWorker {
        checkOpen()
        require(directory.isAbsolute && directory.path.isNotBlank() && '\u0000' !in directory.path && directory.path.toByteArray().size <= 4096)
        val fd = Os.open(directory.path, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        val packageBytes = try {
            ParcelFileDescriptor.dup(fd).use { ExtensionPackage.capture(it.fd) }
        } finally {
            Os.close(fd)
        }
        try {
            var phase = packageBytes.step()
            var steps = 1
            while (phase == ExtensionPackage.Phase.CAPTURING && steps < CAPTURE_STEP_LIMIT) {
                phase = packageBytes.step(); steps++
            }
            check(phase == ExtensionPackage.Phase.CAPTURED) { "固定包快照失败：${packageBytes.info().code}" }
            check(packageBytes.check(hostJSON) == ExtensionPackage.Phase.CHECKED) {
                "插件包与 Android Client 能力不兼容：${packageBytes.info().code}"
            }
            val metadata = packageBytes.metadata()
            val key = Key(metadata.id, metadata.packageDigest)
            check(key !in records) { "相同 ID 与摘要的包已审查" }
            val value = Review(metadata.id, metadata.version, metadata.publisher, metadata.license,
                metadata.packageDigest, metadata.runtime, metadata.requires.toList(), metadata.optionalMissing.toList(),
                metadata.authorizationGranted, metadata.publisherAuthenticated,
                packageBytes.commands().map { Command(it.path, it.title, it.argsSchema, it.views) })
            records[key] = Record(packageBytes, value, Source.UNKNOWN)
            value
        } catch (error: Throwable) {
            runCatching { packageBytes.close() }
            throw error
        }
    }

    /** A SAF tree is copied as an exact bounded byte set into private cache,
     * then Core captures that private directory by fd. Provider metadata is
     * never treated as the package snapshot. The staging directory is removed
     * whether validation succeeds, fails, or the caller cancels. */
    suspend fun reviewTree(tree: Uri): Review {
        val staged = withContext(Dispatchers.IO) { stageTree(resolver, tree, stagingRoot) }
        return try { review(staged) } finally {
            withContext(NonCancellable + Dispatchers.IO) {
                check(staged.deleteRecursively() || !staged.exists()) { "插件暂存目录清理失败" }
            }
        }
    }

    suspend fun approve(id: String, digest: String): ExtensionRegistry.Status = setSource(id, digest, Source.ALLOWED)
    suspend fun deny(id: String, digest: String): ExtensionRegistry.Status = setSource(id, digest, Source.DENIED)

    suspend fun resolve(command: String, selectedID: String? = null, selectedDigest: String? = null): Resolution = onWorker {
        checkOpen()
        if (!validCommand(command) || (selectedID == null) != (selectedDigest == null)) {
            return@onWorker Resolution(ExtensionRegistry.Status.INVALID)
        }
        val candidates = records.values.filter { record ->
            record.source != Source.DENIED && record.review.commands.any { overlaps(command, it.path) }
        }
        if (selectedID == null && candidates.size > 1) return@onWorker Resolution(ExtensionRegistry.Status.CONFLICT)
        val chosen = if (selectedID != null) records[Key(selectedID, selectedDigest!!)]
            ?: return@onWorker Resolution(ExtensionRegistry.Status.STALE)
        else candidates.firstOrNull()
        when (chosen?.source) {
            Source.UNKNOWN -> return@onWorker Resolution(ExtensionRegistry.Status.UNKNOWN, chosen.review.id, chosen.review.digest)
            Source.DENIED -> return@onWorker Resolution(ExtensionRegistry.Status.DENIED, chosen.review.id, chosen.review.digest)
            else -> Unit
        }
        val selection = registryOnWorker().resolve(command, executor = false,
            selectedId = selectedID ?: chosen?.review?.id, selectedDigest = selectedDigest ?: chosen?.review?.digest)
        val selectedPackage = selection.packageBytes ?: return@onWorker Resolution(selection.status)
        try {
            val metadata = selectedPackage.metadata()
            val contribution = selection.command ?: return@onWorker Resolution(ExtensionRegistry.Status.NOT_FOUND)
            Resolution(ExtensionRegistry.Status.OK, metadata.id, metadata.packageDigest,
                Command(contribution.path, contribution.title, contribution.argsSchema, contribution.views))
        } finally {
            selectedPackage.close()
        }
    }

    suspend fun close() {
        closed.set(true)
        if (cleanupComplete.get()) return
        try {
            withContext(NonCancellable + dispatcher) {
                if (cleanupComplete.get()) return@withContext
                checkWorker()
                // Close consumers before the transport/package owners they borrow.
                runtime?.let { current -> current.close(); runtime = null; runtimePackageKey = null; runtimeCommand = null }
                session?.let { current -> current.close(); session = null; sessionPackageKey = null }
                // Channel close can retain its native owner while it drains.
                // Do not release packages or stop this worker until a retry
                // actually reports that the channel owner is gone.
                channel?.let { current -> current.close(); channel = null }

                var failure: Throwable? = null
                fun recordFailure(next: Throwable) {
                    val previous = failure
                    if (previous == null) failure = next else previous.addSuppressed(next)
                }
                // Registry owns references to approved packages; release it first.
                registry?.let { current ->
                    val status = current.closeStatus()
                    registry = null
                    if (status != ExtensionRegistry.Status.OK) {
                        recordFailure(IllegalStateException("插件注册表关闭失败：$status"))
                    }
                }
                val iterator = records.iterator()
                while (iterator.hasNext()) {
                    val record = iterator.next().value
                    try {
                        record.packageBytes.close()
                        iterator.remove()
                    } catch (error: Throwable) {
                        recordFailure(error)
                        if (!record.packageBytes.hasOwner) iterator.remove()
                    }
                }
                if (records.isNotEmpty()) {
                    val error = IllegalStateException("仍有插件包 owner 未释放；保留 controller worker 供重试")
                    failure?.let(error::addSuppressed)
                    throw error
                }
                cleanupComplete.set(true)
                failure?.let { throw it }
            }
        } finally {
            if (cleanupComplete.get()) {
                thread.quitSafely()
            }
        }
    }

    private suspend fun setSource(id: String, digest: String, source: Source): ExtensionRegistry.Status = onWorker {
        checkOpen()
        val record = records[Key(id, digest)] ?: return@onWorker ExtensionRegistry.Status.STALE
        if (source == Source.ALLOWED) {
            val status = registryOnWorker().put(record.packageBytes, ExtensionRegistry.Source.ALLOWED,
                if (record.source == Source.ALLOWED) digest else null)
            if (status == ExtensionRegistry.Status.OK) record.source = Source.ALLOWED
            status
        } else {
            if (record.source == Source.ALLOWED) {
                val status = registryOnWorker().remove(id, executor = false, expectedDigest = digest)
                if (status != ExtensionRegistry.Status.OK) return@onWorker status
            }
            record.source = Source.DENIED
            ExtensionRegistry.Status.DENIED
        }
    }

    private fun registryOnWorker(): ExtensionRegistry {
        checkOpen(); checkWorker()
        return registry ?: ExtensionRegistry().also { registry = it }
    }

    private fun checkOpen() { check(!closed.get()) { "插件控制器已关闭" }; checkWorker() }
    private fun checkWorker() { check(Thread.currentThread() === thread && Looper.myLooper() != Looper.getMainLooper()) }
    private suspend fun <T> onWorker(block: () -> T): T = withContext(dispatcher) { checkOpen(); block() }

    private fun overlaps(lhs: String, rhs: String) = lhs == rhs || lhs.startsWith("$rhs ") || rhs.startsWith("$lhs ")

    companion object {
        private const val CAPTURE_STEP_LIMIT = 100_000
        private const val TREE_ENTRY_LIMIT = 4096
        private const val TREE_MEMBER_BYTES = 16L * 1024L * 1024L
        private const val TREE_TOTAL_BYTES = 64L * 1024L * 1024L
        private const val TREE_DEPTH_LIMIT = 64
        private fun validName(value: String) = value.isNotBlank() && '\u0000' !in value && value.toByteArray().size <= 128
        private fun validCommand(value: String): Boolean {
            if (value.isEmpty() || value.toByteArray().size > 519) return false
            val parts = value.split(' ')
            return parts.size in 1..8 && parts.all { it.length in 1..64 && it.matches(Regex("[a-z][a-z0-9-]*")) }
        }

        private data class TreeBudget(var entries: Int = 0, var bytes: Long = 0, val seenDocuments: MutableSet<String> = mutableSetOf())

        private suspend fun stageTree(resolver: ContentResolver, tree: Uri, parent: File): File {
            require(DocumentsContract.isTreeUri(tree) && tree.authority != null) { "请选择一个文档文件夹" }
            check(parent.exists() || parent.mkdirs()) { "无法创建私有插件暂存目录" }
            val target = File(parent, UUID.randomUUID().toString())
            check(target.mkdir()) { "无法创建本次插件暂存目录" }
            try {
                val rootId = DocumentsContract.getTreeDocumentId(tree)
                val budget = TreeBudget()
                require(budget.seenDocuments.add(rootId))
                copyDirectory(resolver, tree, rootId, target, "", 0, budget)
                return target
            } catch (error: Throwable) {
                if (!target.deleteRecursively() && target.exists()) {
                    error.addSuppressed(IllegalStateException("插件暂存目录清理失败"))
                }
                throw error
            }
        }

        private suspend fun copyDirectory(resolver: ContentResolver, tree: Uri, documentId: String,
            destination: File, prefix: String, depth: Int, budget: TreeBudget) {
            currentCoroutineContext().ensureActive()
            require(depth <= TREE_DEPTH_LIMIT) { "插件目录嵌套过深" }
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_FLAGS,
            )
            resolver.query(childrenUri, projection, null, null, null).use { cursor ->
                requireNotNull(cursor) { "无法读取所选插件文件夹" }
                val idColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val mimeColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
                val flagsColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_FLAGS)
                while (cursor.moveToNext()) {
                    currentCoroutineContext().ensureActive()
                    budget.entries++
                    require(budget.entries <= TREE_ENTRY_LIMIT) { "插件目录条目过多" }
                    val name = cursor.getString(nameColumn) ?: error("插件成员缺少名称")
                    require(validSegment(name)) { "插件成员名称无效" }
                    val relative = if (prefix.isEmpty()) name else "$prefix/$name"
                    require(relative.toByteArray().size <= 4096) { "插件成员路径过长" }
                    val id = cursor.getString(idColumn) ?: error("插件成员缺少文档标识")
                    require(budget.seenDocuments.add(id)) { "文档提供方返回重复成员" }
                    val mime = cursor.getString(mimeColumn)
                    val flags = cursor.getInt(flagsColumn)
                    require((flags and DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT) == 0) {
                        "不接受由文档提供方转换生成的虚拟文件"
                    }
                    val childUri = DocumentsContract.buildDocumentUriUsingTree(tree, id)
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                        val childDirectory = File(destination, name)
                        check(childDirectory.mkdir()) { "无法创建插件目录成员" }
                        copyDirectory(resolver, tree, id, childDirectory, relative, depth + 1, budget)
                    } else {
                        copyFile(resolver, childUri, File(destination, name), budget)
                    }
                }
            }
        }

        private suspend fun copyFile(resolver: ContentResolver, uri: Uri, target: File, budget: TreeBudget) {
            check(target.createNewFile()) { "插件成员路径重复" }
            val input = resolver.openInputStream(uri) ?: error("无法读取插件成员")
            input.use { source -> FileOutputStream(target, false).use { output ->
                val buffer = ByteArray(16 * 1024)
                var memberBytes = 0L
                while (true) {
                    currentCoroutineContext().ensureActive()
                    val count = source.read(buffer)
                    if (count < 0) break
                    require(count > 0 && memberBytes <= TREE_MEMBER_BYTES - count && budget.bytes <= TREE_TOTAL_BYTES - count) {
                        "插件包内容超过暂存上限"
                    }
                    output.write(buffer, 0, count)
                    memberBytes += count
                    budget.bytes += count
                }
            } }
        }

        private fun validSegment(value: String) = value.isNotEmpty() && value != "." && value != ".." &&
            '\u0000' !in value && '/' !in value && '\\' !in value && value.toByteArray().size <= 255
    }
}

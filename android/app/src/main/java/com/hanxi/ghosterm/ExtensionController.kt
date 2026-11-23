package com.hanxi.ghosterm

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
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
import com.hanxi.ghosterm.bridge.ExtensionChannelException
import com.hanxi.ghosterm.bridge.ExtensionMultiplexer
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionFileSource
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import com.hanxi.ghosterm.bridge.SSHAuthState
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.android.asCoroutineDispatcher
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.Inet6Address
import java.net.InetAddress
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

internal fun luaVersionDirectory(context: Context) =
    File(context.applicationContext.filesDir, "lua-plugin-releases")

/** App-side Client package owner. Native package and registry calls stay on one
 * non-UI worker. Reviewing never grants source trust or activates Lua. */
class ExtensionController private constructor(private val shared: Shared, private val ownsLibrary: Boolean,
    private val parent: ExtensionController? = null) {
    constructor(profile: HostProfile, context: Context,
        versionDirectory: File = luaVersionDirectory(context)) : this(Shared(profile, context, versionDirectory), true)
    internal constructor(profile: HostProfile, context: Context, versionDirectory: File,
        channelStore: PluginChannelPolicyStore, channelClock: (() -> ULong)? = null) :
        this(Shared(profile, context, versionDirectory, channelStore, channelClock), true)

    internal constructor(profile: HostProfile, context: Context, refresh: PluginChannelRefresh) :
        this(Shared(profile, context, luaVersionDirectory(context),
            refresh.store, channelRefresh = refresh), true)

    internal val hasConfiguredChannel: Boolean get() = shared.channelStore != null
    internal val canRefreshChannelPolicy: Boolean get() = shared.channelRefresh != null
    internal suspend fun refreshChannelPolicy() =
        checkNotNull(shared.channelRefresh) { "本构建未配置插件渠道。" }.refresh()

    internal suspend fun cachedChannelPolicy(recordObservation: Boolean = true): PluginReleaseSignature.VerifiedChannelPolicy = onWorker {
        checkOpen()
        checkNotNull(shared.channelStore) { "本构建未配置插件渠道。" }.useCurrent(configuredChannelTime(), recordObservation = recordObservation) { it }
    }

    internal suspend fun runningInstancesWithRevokedPublisher(policy: PluginReleaseSignature.VerifiedChannelPolicy): Int = onWorker {
        checkOpen()
        shared.controllers.count { controller ->
            val admission = controller.channelAdmission
            admission != null && (controller.runtime != null || controller.session != null) &&
                policy.revokesPublisher(admission.root, admission.proof)
        }
    }

    internal suspend fun currentInstanceHasRevokedPublisher(): Boolean = onWorker {
        checkOpen()
        val admission = channelAdmission ?: return@onWorker false
        val store = shared.channelStore ?: return@onWorker false
        if (session == null && runtime == null) return@onWorker false
        store.useCurrent(configuredChannelTime()) { it.revokesPublisher(admission.root, admission.proof) }
    }

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

    /** Local Client grant, deliberately separate from the remote Relay Request. */
    data class ClientWorkspace(val directory: String, val authorized: Boolean) {
        fun validate(requiredCapabilities: List<String>) {
            require(authorized && Charsets.UTF_8.newEncoder().canEncode(directory) && directory.startsWith('/') && directory.toByteArray(Charsets.UTF_8).size <= 4096 &&
                directory.none { it == '\u0000' || it == '\n' || it == '\r' } &&
                "resource.receive" in requiredCapabilities) {
                "本机资源目录需要显式允许，且 Client 必须声明 resource.receive"
            }
        }
        val reviewDescription get() = "Native 只读发布目录：$directory · 单文件上限 256 KiB"
    }

    data class Command(val path: String, val title: String, val argsSchema: String, val views: List<String>,
                       val togglePanel: Boolean = false)
    data class Review(
        val id: String, val version: String, val publisher: String, val license: String,
        val digest: String, val runtime: String, val requiredCapabilities: List<String>,
        val optionalMissingCapabilities: List<String>, val authorizationGranted: Boolean,
        val publisherAuthenticated: Boolean,
        val commands: List<Command>, val sourceApproved: Boolean = false,
    )
    internal data class SignedArchiveReview(val review: Review, val proof: PluginReleaseSignature.Proof,
        val declarations: PluginReleaseDeclarations.Data, val channelStatementSHA256: String? = null,
        val channelGeneration: ULong? = null)
    internal data class InstalledRelease(val snapshot: SignedArchiveReview, val publication: PluginReleaseStage.Publication)
    internal enum class PreviousVersionState { NONE, VERIFIED, UNKNOWN }
    internal data class VersionPreview(val id: String, val revision: String?, val releaseID: String,
        val candidate: SignedArchiveReview, val previous: SignedArchiveReview?, val previousState: PreviousVersionState,
        val previousReleaseID: String?, val previousEnabled: Boolean?) {
        val addedCapabilities get() = if (previousState == PreviousVersionState.UNKNOWN) null else candidate.review.requiredCapabilities.filter { it !in previous?.review?.requiredCapabilities.orEmpty() }
        val removedCapabilities get() = if (previousState == PreviousVersionState.UNKNOWN) null else previous?.review?.requiredCapabilities.orEmpty().filter { it !in candidate.review.requiredCapabilities }
        val addedPermissions get() = if (previousState == PreviousVersionState.UNKNOWN) null else candidate.declarations.permissions.filter { it !in previous?.declarations?.permissions.orEmpty() }
        val removedPermissions get() = if (previousState == PreviousVersionState.UNKNOWN) null else previous?.declarations?.permissions.orEmpty().filter { it !in candidate.declarations.permissions }
    }
    internal data class VersionSelectionResult(val pluginID: String, val review: Review?, val observation: PluginReleaseCatalog.Observation,
        val durabilityConfirmed: Boolean)
    data class Resolution(
        val status: ExtensionRegistry.Status,
        val packageID: String? = null,
        val packageDigest: String? = null,
        val command: Command? = null,
    )
    data class ChannelState(val info: ExtensionChannel.Info, val authentication: SSHAuthState)
    data class SessionState(val packageID: String, val packageDigest: String, val info: ExtensionSession.Info)
    data class ResourcePublication(val id: String, val relativePath: String, val phase: ExtensionFileSource.Phase,
        val error: ExtensionFileSource.Status, val cleanupError: ExtensionFileSource.Status = ExtensionFileSource.Status.OK,
        val confirmed: Boolean = false, val released: Boolean = false, val expired: Boolean = false,
        val sha256: String = "", val bytes: Long = 0, val accepted: Long = 0) {
        val reviewDescription: String get() {
            val message = when (phase) {
                ExtensionFileSource.Phase.HASHING -> "正在校验文件"
                ExtensionFileSource.Phase.READY, ExtensionFileSource.Phase.SENDING -> "正在发送：$accepted / $bytes 字节"
                ExtensionFileSource.Phase.RECEIPT -> "等待对端确认"
                ExtensionFileSource.Phase.CANCELLING -> "正在取消，结果尚未确认"
                ExtensionFileSource.Phase.SUCCEEDED -> if (confirmed) "发布完成，对端已确认" else "发布结果尚未确认"
                ExtensionFileSource.Phase.STOPPED -> "发布已停止"
                ExtensionFileSource.Phase.UNCERTAIN -> "发布结果不明"
                ExtensionFileSource.Phase.FAILED -> "发布失败（$error）"
            }
            return message + (if (expired) " · 已到期" else "") + (if (cleanupError == ExtensionFileSource.Status.OK) "" else " · 资源释放失败")
        }
    }
    data class RuntimeState(val packageID: String, val packageDigest: String, val command: String, val info: ExtensionRuntime.Info,
        val publication: ResourcePublication? = null, val instanceID: String = "",
        val views: ByteArray? = null, val viewSessionInstance: String? = null, val viewSessionEpoch: String? = null,
        val viewIssue: String? = null, val stopRequested: Boolean = false)

    internal data class PolicyDisableResult(val pluginIDs: List<String>, val observation: PluginReleaseCatalog.Observation,
        val durabilityConfirmed: Boolean)
    private data class ChannelAdmission(val root: String, val proof: PluginReleaseSignature.Proof)
    private data class Key(val id: String, val digest: String)
    private data class Record(val packageBytes: ExtensionPackage, val review: Review, var source: Source,
        var managedReleaseID: String? = null, var selectionGeneration: String? = null, var approvalGeneration: String? = null,
        var rootIdentity: PluginReleaseCatalog.Identity? = null, var versionIdentity: PluginReleaseCatalog.Identity? = null,
        var payloadIdentity: PluginReleaseCatalog.Identity? = null)
    private data class PendingVersion(val value: VersionPreview, val policy: ByteArray, val root: PluginReleaseCatalog.Identity,
        val version: PluginReleaseCatalog.Version, val payload: PluginReleaseCatalog.Identity) : AutoCloseable {
        override fun close() = version.close()
    }
    private data class CheckedVersion(val packageBytes: ExtensionPackage, val value: SignedArchiveReview, val payload: PluginReleaseCatalog.Identity)

    private class Shared(profile: HostProfile, context: Context, val versionDirectory: File,
        val channelStore: PluginChannelPolicyStore? = null, val channelClock: (() -> ULong)? = null,
        val channelRefresh: PluginChannelRefresh? = null) {
        val thread = HandlerThread("ghosterm-extension-controller", Thread.NORM_PRIORITY).apply { start() }
        val handler = Handler(thread.looper)
        val dispatcher: CoroutineDispatcher = handler.asCoroutineDispatcher("ghosterm-extension-controller")
        val appContext = context.applicationContext
        val releaseProfile = profile.copy(protocols = profile.protocols.toList(), runtimes = profile.runtimes.toList(), capabilities = profile.capabilities.toList())
        val closing = AtomicBoolean(false)
        val controllers = linkedSetOf<ExtensionController>()
        val hostJSON = releaseProfile.json()
        val hostCapabilities = releaseProfile.capabilities
        val records = linkedMapOf<Key, Record>()
        val managedIDs = mutableSetOf<String>()
        val registeredDigests = mutableMapOf<String, String>()
        var policyRequest: String? = null
        var pendingPolicyReview: PluginPolicyReview? = null
        var pendingVersion: PendingVersion? = null
        var pendingRemoval: Pair<String, PluginReleaseRemoval>? = null
        var registry: ExtensionRegistry? = null
    }
    private val thread get() = shared.thread
    private val dispatcher get() = shared.dispatcher
    private val appContext get() = shared.appContext
    private val releaseProfile get() = shared.releaseProfile
    private val versionDirectory get() = shared.versionDirectory
    private val hostJSON get() = shared.hostJSON
    private val hostCapabilities get() = shared.hostCapabilities
    private val records get() = shared.records
    private val managedIDs get() = shared.managedIDs
    private val registeredDigests get() = shared.registeredDigests
    private var policyRequest: String?
        get() = shared.policyRequest
        set(value) { shared.policyRequest = value }
    private var pendingPolicyReview: PluginPolicyReview?
        get() = shared.pendingPolicyReview
        set(value) { shared.pendingPolicyReview = value }
    private var pendingVersion: PendingVersion?
        get() = shared.pendingVersion
        set(value) { shared.pendingVersion = value }
    private var pendingRemoval: Pair<String, PluginReleaseRemoval>?
        get() = shared.pendingRemoval
        set(value) { shared.pendingRemoval = value }
    private var registry: ExtensionRegistry?
        get() = shared.registry
        set(value) { shared.registry = value }

    init { shared.controllers.add(this) }
    /** An independent worker keeps archive IO away from an active Client pump. */
    internal fun newReleaseInspector(): ExtensionController {
        check(!closed.get()) { "插件控制器已关闭" }
        val store = shared.channelStore
        return if (store == null) ExtensionController(releaseProfile, appContext, versionDirectory)
        else ExtensionController(releaseProfile, appContext, versionDirectory, store, shared.channelClock)
    }
    /** Explicit read-only observation. Absence and unreadable state are separate;
     * never captures a package, registers a source or loads a saved selection. */
    internal suspend fun installedVersions(directory: File = versionDirectory): PluginReleaseCatalog.Observation = onWorker {
        PluginReleaseCatalog.open(directory)?.use { it.observe() }
            ?: PluginReleaseCatalog.Observation(emptyList(), emptyList(), 0, PluginReleaseCatalog.Selection(null, emptyMap()), null)
    }
    private val resolver: ContentResolver = appContext.contentResolver
    private val resourceDirectory = File(appContext.cacheDir, "plugin-resources-${UUID.randomUUID()}")
    private val stagingRoot = File(appContext.cacheDir, "extension-package-review")
    private val closed = AtomicBoolean(false)
    private val cleanupComplete = AtomicBoolean(false)
    private var multiplexer: ExtensionMultiplexer? = null
    private var muxPump: Runnable? = null
    private var muxDrainDeadline = 0L
    private var finalMultiplexState: MultiplexState? = null
    private var nativeConnection: String? = null
    private var nativePending: String? = null
    private var nativeAbandoned = false
    private var connectedTarget = ""
    @Volatile private var channel: ExtensionChannel? = null // stop alone may cross threads
    private var foregroundInspectionReader: Any? = null
    @Volatile private var session: ExtensionSession? = null
    private var sessionPackageKey: Key? = null
    private var sessionApprovedSnapshot = false
    private var channelAdmission: ChannelAdmission? = null
    private var sessionVersionLease: ParcelFileDescriptor? = null
    @Volatile private var runtime: ExtensionRuntime? = null
    private var runtimePackageKey: Key? = null
    private var runtimeVersionLease: ParcelFileDescriptor? = null
    private var runtimeInstanceID: String? = null
    private var runtimeCommand: String? = null
    private var runtimeViewScope: Pair<String, String>? = null
    private var runtimeViewIssue: String? = null
    private var clientRootFD: java.io.FileDescriptor? = null
    private var clientSource: ExtensionFileSource? = null
    private var clientPublication: ResourcePublication? = null
    private var clientPublicationDeadline = 0L
    private var resourceStore: ExtensionResourceStore? = null

    data class MultiplexScope(val instance: String, val epoch: ULong)
    data class MultiplexState(val info: ExtensionMultiplexer.Info, val transport: ExtensionChannel.Info)

    suspend fun makeInvocationController(): ExtensionController = onWorker {
        check(shared.controllers.size < 145) { "插件运行 owner 数量已达上限" }
        ExtensionController(shared, false)
    }
    suspend fun adoptMultiplexConnection(connection: String, scopes: List<MultiplexScope>, live: Boolean = false): List<ExtensionController> = onWorker {
        require(scopes.size in 1..8 && scopes.map { it.instance }.distinct().size == scopes.size && scopes.map { it.epoch }.distinct().size == scopes.size)
        check(parent == null && multiplexer == null && session == null && runtime == null && shared.controllers.size + scopes.size <= 145)
        val mux = ExtensionMultiplexer.adopt(checkNotNull(channel), connection, frameBytes = if (live) 8192 else 4096)
        channel = null; multiplexer = mux
        if (live) { mux.enableNativeControl(true); nativeConnection = connection }
        val children = scopes.map { scope ->
            ExtensionController(shared, false, this).also { it.channel = mux.openChannel(scope.instance, scope.epoch) }
        }
        val pump = object : Runnable {
            override fun run() {
                if (multiplexer !== mux) return
                try { if (mux.step() != 0) mux.stop() } catch (_: Exception) { mux.stop() }
                if (nativeAbandoned && mux.takeNativeControl() != null) { nativePending = null; nativeAbandoned = false }
                shared.handler.postDelayed(this, 2)
            }
        }
        muxPump = pump; shared.handler.post(pump)
        children
    }
    suspend fun openMultiplexInstance(scope: MultiplexScope): ExtensionController = onWorker {
        check(nativeConnection != null && shared.controllers.size < 145)
        val child = ExtensionController(shared, false, this)
        try { child.channel = checkNotNull(multiplexer).openChannel(scope.instance, scope.epoch); child }
        catch (failure: Exception) { shared.controllers.remove(child); throw failure }
    }
    suspend fun stopMultiplexConnection() = withContext(NonCancellable + dispatcher) { checkWorker(); multiplexer?.stop() }
    suspend fun multiplexTargetDescription(): String = onWorker { connectedTarget }
    suspend fun manageMultiplex(method: String, input: String = "{}"): String {
        val caller = currentCoroutineContext()
        caller.ensureActive()
        val ticket = withContext(NonCancellable) { onWorker {
            check(method in listOf("hello", "open", "status", "finish") && input.toByteArray().size <= 6144 && nativePending == null)
            val value = JSONObject(input); val mux = checkNotNull(multiplexer); val connection = checkNotNull(nativeConnection)
            val id = UUID.randomUUID().toString().replace("-", "")
            val frame = JSONObject().put("wireVersion", 1).put("connectionId", connection).put("instanceId", connection)
                .put("epoch", "1").put("streamId", "rpc.request").put("seq", mux.nativeInfo().nextSend.toString()).put("kind", "request")
                .put("payload", JSONObject().put("requestId", id).put("method", "native.multiplex." + method).put("timeoutMs", 10000).put("input", value))
            mux.sendNativeControl(frame.toString().toByteArray()); nativePending = id; nativeAbandoned = false; id
        } }
        try {
            val result = withContext(NonCancellable) {
                val deadline = SystemClock.elapsedRealtime() + 10000
                var value: String? = null
                while (value == null) {
                    value = onWorker {
                        check(nativePending == ticket)
                        val mux = checkNotNull(multiplexer); mux.step()
                        val data = mux.takeNativeControl()
                        if (data == null) { check(mux.info().error == 0) { "连接中断；管理结果未确认" }; null }
                        else {
                            val payload = JSONObject(data.toString(Charsets.UTF_8)).getJSONObject("payload")
                            check(payload.getString("requestId") == ticket)
                            val outcome = payload.getJSONObject("outcome"); nativePending = null
                            check(outcome.getString("kind") == "success") { "连接管理拒绝：${outcome.optJSONObject("error")?.optString("code") ?: "UNKNOWN"}" }
                            outcome.getJSONObject("value").toString()
                        }
                    }
                    check(value != null || SystemClock.elapsedRealtime() < deadline) { "连接管理结果未确认；不会重放请求" }
                    if (value == null) delay(10)
                }
                value
            }
            caller.ensureActive(); return result
        } catch (failure: Exception) {
            withContext(NonCancellable) { onWorker { if (nativePending == ticket) nativeAbandoned = true } }
            throw failure
        }
    }
    private fun multiplexState(mux: ExtensionMultiplexer) = MultiplexState(mux.info(), mux.transportInfo())
    suspend fun stepMultiplexConnection(): MultiplexState = onWorker {
        val mux = checkNotNull(multiplexer); mux.step(); multiplexState(mux)
    }
    suspend fun multiplexObservation(): MultiplexState? = withContext(dispatcher) {
        checkWorker(); multiplexer?.let { multiplexState(it) } ?: finalMultiplexState
    }

    /** Create an explicitly requested gterm relay route. This does not install,
     * discover, or start a remote Executor; the caller supplies its exact command. */
    suspend fun openSSHChannel(hops: List<ExtensionChannel.Hop>, family: Int, address: ByteArray,
        scope: Int = 0, bootstrap: String): ChannelState = onWorker {
        check(parent == null && multiplexer == null && channel == null) { "已有插件通道占用此控制器" }
        connectedTarget = hops.joinToString(" → ") { "${it.user}@${it.host}:${it.port}" }
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

    /** Reads only bounded inspection metadata on the native worker. EOF and a
     * packet-backed zero exit are both required; never adopts a Session. */
    suspend fun readForegroundDeclaration(instance: String): PluginForegroundDeclaration {
        val reader = Any()
        val output = java.io.ByteArrayOutputStream()
        val deadline = SystemClock.elapsedRealtime() + 8000
        try {
            // Acquisition itself may complete on the worker while the caller
            // is cancelled before withContext resumes. The ticket is created
            // before suspension, and only its own finally may release it.
            val current = onWorker {
                check(foregroundInspectionReader == null && session == null && runtime == null)
                checkNotNull(channel).also { foregroundInspectionReader = reader }
            }
            while (true) {
                currentCoroutineContext().ensureActive()
                check(SystemClock.elapsedRealtime() < deadline) { "前台实例检查超时" }
                val value = onWorker {
                    check(channel === current && session == null && runtime == null) { "前台检查通道已失效" }
                    current.step(0)
                    val (io, bytes) = current.read()
                    check((io.data || io.wait || io.eof) && bytes.size <= PluginForegroundDeclaration.MAXIMUM_BYTES - output.size()) {
                        "前台检查通道失败或声明超过上限"
                    }
                    output.write(bytes)
                    val info = current.info()
                    if (io.eof && info.exitStatus != null) {
                        check(info.exitStatus == 0 && info.error == 0 && info.cleanupError == 0) { "前台实例未确认在线或声明读取失败" }
                        PluginForegroundDeclaration.parse(output.toByteArray(), instance)
                    } else null
                }
                if (value != null) return value
                delay(10)
            }
        } finally {
            withContext(NonCancellable) { onWorker {
                if (foregroundInspectionReader === reader) foregroundInspectionReader = null
                output.reset()
            } }
        }
    }

    /** Transfer a ready SSH relay to a HOST Session using only the already
     * approved fixed Client package and caller-selected Executor identity. */
    suspend fun adoptSSHSession(pluginID: String, packageDigest: String, executorDigest: String,
        hostVersion: String, peerIdentity: String, methods: List<ExtensionSession.Method>,
        additionalRequiredCapabilities: List<String> = emptyList()): SessionState = onWorker {
        managedAccess { owner, choices ->
        check(session == null && runtime == null) { "此 controller 已有活动插件 Session" }
        val key = Key(pluginID, packageDigest)
        val record = records[key] ?: error("插件包摘要已失效")
        check(record.source == Source.ALLOWED) { "插件来源尚未获本进程批准" }
        val admission = managedStatus(record, owner, choices, approval = true)
        if (admission != ExtensionRegistry.Status.OK) throw ExtensionRegistry.RegistryException(admission)
        require(executorDigest.matches(Regex("[a-f0-9]{64}"))) { "Executor 摘要无效" }
        require(hostVersion.isNotBlank() && hostVersion.toByteArray().size <= 64 && '\u0000' !in hostVersion)
        require(peerIdentity.isNotBlank() && peerIdentity.toByteArray().size <= 255 && '\u0000' !in peerIdentity)
        val bytes = checkNotNull(channel) { "插件 SSH 通道尚未打开" }
        check(bytes.info().phase == 1) { "插件 SSH 通道尚未就绪" }
        val metadata = record.packageBytes.metadata()
        check(metadata.id == pluginID && metadata.packageDigest == packageDigest && !metadata.executor) {
            "固定包不是所选 Client 包"
        }
        val remoteProcessCapabilities = listOf("process.spawn", "process.pipes")
        val requestedRemoteProcess = additionalRequiredCapabilities.any { it in remoteProcessCapabilities }
        val remoteProcessPairComplete = remoteProcessCapabilities.all { it in additionalRequiredCapabilities }
        check(metadata.requires.size + additionalRequiredCapabilities.size <= 16 &&
            additionalRequiredCapabilities.all { it in hostCapabilities || it in remoteProcessCapabilities } &&
            (!requestedRemoteProcess || remoteProcessPairComplete) &&
            (metadata.requires + additionalRequiredCapabilities).distinct().size ==
                metadata.requires.size + additionalRequiredCapabilities.size) {
            "附加 Session 能力必须由 Host 支持且不能重复"
        }
        val receiveStore = if ("resource.receive" in metadata.requires) resourceStoreOnWorker() else null
        val sessionAvailable = hostCapabilities + remoteProcessCapabilities.filter {
            requestedRemoteProcess && it !in hostCapabilities
        }
        val version = if (record.review.id in managedIDs) checkNotNull(owner).openVersion(checkNotNull(record.managedReleaseID)) else null
        version.use {
        withChannelAdmission(version, record) { admission ->
        var reference = version?.let { it.pin(); it.borrow() }
        try {
        val owned = ExtensionSession.adopt(bytes, ExtensionSession.Role.HOST,
            ExtensionSession.Config(metadata.id, metadata.version, metadata.packageDigest, executorDigest,
                hostVersion, metadata.sdkVersion, metadata.requires + additionalRequiredCapabilities, sessionAvailable,
                budgets = ExtensionSession.Budgets(resourceBytes = RESOURCE_BYTES.toInt()),
                peerKind = ExtensionSession.PeerKind.SSH, peerIdentity = peerIdentity, methods = methods.toList()), resourceStore = receiveStore,
            resourceScopeBytes = RESOURCE_SESSION_BYTES)
        channel = null; session = owned; sessionPackageKey = key
        sessionApprovedSnapshot = true; sessionVersionLease = reference; reference = null
        channelAdmission = admission
        SessionState(pluginID, packageDigest, owned.info())
        } finally { reference?.close() }
        }
        }
        }
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
        val key = checkNotNull(sessionPackageKey)
        fun decide() {
            check(current.decide(consentToken, decision) == 0) { "插件授权决定未被接收" }
        }
        if (decision == ExtensionSession.Decision.ALLOW && channelAdmission != null) {
            val record = checkNotNull(records[key]) { "固定插件包已失效" }
            val lease = checkNotNull(sessionVersionLease) { "固定插件版本已失效" }
            val version = PluginReleaseCatalog.Version(ParcelFileDescriptor.dup(lease.fileDescriptor),
                checkNotNull(record.managedReleaseID))
            version.use {
                withChannelAdmission(it, record) { admission ->
                    if (admission == null) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
                    decide()
                }
            }
        } else decide()
        SessionState(key.id, key.digest, current.info())
    }

    /** Activate only after native Session consent reaches ACTIVE. The command
     * is resolved against the exact approved package snapshot before runtime
     * adopts the Session. */
    suspend fun activateClient(pluginID: String, packageDigest: String, command: String,
        arguments: List<String>, clientWorkspace: ClientWorkspace? = null,
        context: ExtensionRuntime.Invocation.Context? = null): RuntimeState = onWorker {
        val current = checkNotNull(session) { "插件 Session 尚未接管" }
        check(current.info().phase == ExtensionSession.Phase.ACTIVE) { "插件 Session 尚未获得授权" }
        check(sessionPackageKey == Key(pluginID, packageDigest)) { "插件 Session 与所选包不匹配" }
        val record = records[Key(pluginID, packageDigest)] ?: error("插件包摘要已失效")
        check(sessionApprovedSnapshot && record.review.commands.any { it.path == command }) {
            "命令不属于已批准的固定插件包"
        }
        val version = sessionVersionLease?.let { lease ->
            PluginReleaseCatalog.Version(ParcelFileDescriptor.dup(lease.fileDescriptor), checkNotNull(record.managedReleaseID))
        }
        version.use {
        withChannelAdmission(version, record) { admission ->
        val registryViews = "ui.views" in record.review.requiredCapabilities
        var viewKinds = 0
        val sameRegistration = registeredDigests[pluginID] == packageDigest
        val selected = if (sameRegistration) registryOnWorker().resolve(command, executor = false, selectedId = pluginID,
            selectedDigest = packageDigest) else null
        val exact = if (sameRegistration) selected?.packageBytes ?: error("命令解析失败：${selected?.status}") else null
        try {
            val contribution = if (sameRegistration) selected?.command else record.packageBytes.commands().singleOrNull { it.path == command }
            check(contribution?.path == command && (exact ?: record.packageBytes).metadata().packageDigest == packageDigest) {
                "命令解析没有返回精确插件包"
            }
            if (registryViews) {
                contribution?.views.orEmpty().forEach { kind ->
                    val bit = when (kind) { "table" -> 1; "diff" -> 2; "task" -> 4; else -> 0 }
                    check(bit != 0 && "ui.$kind" in record.review.requiredCapabilities) { "所选命令没有声明标准视图能力" }
                    viewKinds = viewKinds or bit
                }
                check(viewKinds != 0 && "session.rpc" in record.review.requiredCapabilities) { "视图注册需要标准视图与 RPC" }
            }
        } finally { exact?.close() }
        val sessionInfo = current.info()
        clientWorkspace?.validate(record.review.requiredCapabilities)
        var rootFD = clientWorkspace?.let {
            Os.open(it.directory, OsConstants.O_RDONLY or OsConstants.O_NONBLOCK or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        }
        val active = try {
            rootFD?.let { check(Os.fstat(it).st_mode and OsConstants.S_IFMT == OsConstants.S_IFDIR) { "本机资源路径不是目录" } }
            ExtensionRuntime.adoptPackage(current, record.packageBytes, ExtensionSession.Role.HOST, associated = true,
                cpuMilliseconds = CLIENT_RUNTIME_CPU_MILLISECONDS,
                invocation = ExtensionRuntime.Invocation(command, arguments.toList(), context)).also {
                clientRootFD = rootFD; rootFD = null
            }
        } finally { rootFD?.let(Os::close) }
        session = null; sessionPackageKey = null; runtime = active
        runtimeVersionLease = sessionVersionLease; sessionVersionLease = null; sessionApprovedSnapshot = false
        channelAdmission = admission
        runtimePackageKey = Key(pluginID, packageDigest); runtimeCommand = command; runtimeInstanceID = UUID.randomUUID().toString()
        runtimeViewScope = if (registryViews) sessionInfo.instance to sessionInfo.epoch.toULong().toString() else null
        runtimeViewIssue = null
        if (registryViews) {
            try { active.enableViews(viewKinds) }
            catch (failure: Exception) {
                // Adoption already happened; retain the real Runtime owner for stop/release.
                runtimeViewIssue = "无法启用插件视图注册：${failure.message}"; active.stop()
            }
        }
        runtimeState(active)
        }
        }
    }

    suspend fun takeRuntimeResource(): Pair<ExtensionResourceStore.Status, ExtensionResourceStore.Content?> = onWorker {
        val current = runtime ?: return@onWorker ExtensionResourceStore.Status.CLOSED to null
        current.takeResourceData(RESOURCE_BYTES)
    }

    suspend fun takeRuntimeRequest(): Pair<ExtensionSession.RPCStatus, ExtensionSession.RPCRequest?> = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED to null
        current.takeHostRequest()
    }

    suspend fun respondRuntimeRequest(ticket: Long, outcome: ByteArray): ExtensionSession.RPCStatus = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED
        current.respondHostRequest(ticket, outcome.copyOf())
    }

    internal suspend fun scopedHostRPC(instanceID: String, ticket: Long, outcome: ByteArray? = null): ExtensionSession.RPCStatus = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED
        if (runtimeInstanceID != instanceID || current.info().phase != ExtensionRuntime.Phase.ACTIVE) {
            return@onWorker ExtensionSession.RPCStatus.CLOSED
        }
        val status = current.checkHostRequest(ticket)
        if (status == ExtensionSession.RPCStatus.CLOSED) {
            val cancelled = """{"kind":"cancelled","effect":"none"}""".toByteArray(Charsets.UTF_8)
            current.respondHostRequest(ticket, cancelled)
            return@onWorker status
        }
        if (status == ExtensionSession.RPCStatus.OK && outcome != null) current.respondHostRequest(ticket, outcome) else status
    }

    suspend fun requestRuntimeExecutor(method: String, input: ByteArray,
        timeoutMilliseconds: Int = 30000): ExtensionSession.RPCAdmission {
        val copy = input.copyOf()
        return onWorker {
            val current = runtime ?: return@onWorker ExtensionSession.RPCAdmission(ExtensionSession.RPCStatus.CLOSED, null)
            current.requestExecutor(method, copy, timeoutMilliseconds)
        }
    }

    suspend fun runtimeExecutorRequestResult(request: Long): Pair<ExtensionSession.RPCStatus, ExtensionSession.RPCResult?> = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED to null
        current.executorRequestResult(request)
    }

    suspend fun cancelRuntimeExecutorRequest(request: Long): ExtensionSession.RPCStatus = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED
        current.cancelExecutorRequest(request)
    }

    suspend fun forgetRuntimeExecutorRequest(request: Long): ExtensionSession.RPCStatus = onWorker {
        val current = runtime ?: return@onWorker ExtensionSession.RPCStatus.CLOSED
        current.forgetExecutorRequest(request)
    }

    /** Explicit native UI action using the directory granted to this instance. */
    suspend fun publishClientResource(instanceID: String, relativePath: String, root: String = "client"): RuntimeState = onWorker {
        val current = checkNotNull(runtime)
        check(current.info().phase == ExtensionRuntime.Phase.ACTIVE && runtimeInstanceID == instanceID && clientSource == null) { "资源发布尚未就绪或已有发布" }
        val rootFD = checkNotNull(clientRootFD) { "没有显式允许的本机资源目录" }
        val id = java.util.UUID.randomUUID().toString()
        if (root != "client") {
            clientPublication = ResourcePublication(id, relativePath, ExtensionFileSource.Phase.FAILED,
                ExtensionFileSource.Status.DENIED, released = true)
        } else {
            val admission = ParcelFileDescriptor.dup(rootFD).use { current.openSource(it.fd, relativePath, byteLimit = 262144) }
            clientSource = admission.source
            clientPublicationDeadline = SystemClock.elapsedRealtime() + 10000
            clientPublication = ResourcePublication(id, relativePath,
                if (admission.status == ExtensionFileSource.Status.OK) ExtensionFileSource.Phase.HASHING else ExtensionFileSource.Phase.FAILED,
                admission.status, released = admission.source == null)
        }
        runtimeState(current)
    }
    suspend fun cancelClientPublication(id: String) = onWorker {
        check(clientPublication?.id == id) { "发布不属于当前实例" }
        checkNotNull(clientSource).cancel()
    }
    private fun stepClientPublication() {
        val source = clientSource ?: return
        var publication = checkNotNull(clientPublication)
        if (SystemClock.elapsedRealtime() >= clientPublicationDeadline) {
            publication = publication.copy(expired = true); source.cancel()
        }
        val phase = source.step()
        if (phase == ExtensionFileSource.Phase.READY && !publication.expired) {
            check(source.send() in setOf(ExtensionFileSource.Status.OK, ExtensionFileSource.Status.WAIT)) { "资源发送失败" }
        }
        val info = source.info()
        publication = publication.copy(phase = info.phase, error = info.error, cleanupError = info.cleanupError,
            confirmed = info.transfer.confirmed, sha256 = info.descriptor.sha256, bytes = info.descriptor.size, accepted = info.accepted)
        if (info.phase in setOf(ExtensionFileSource.Phase.SUCCEEDED, ExtensionFileSource.Phase.FAILED,
                ExtensionFileSource.Phase.STOPPED, ExtensionFileSource.Phase.UNCERTAIN)) {
            when (val closed = source.closeStep()) {
                ExtensionFileSource.Status.OK -> { clientSource = null; publication = publication.copy(released = true) }
                ExtensionFileSource.Status.WAIT -> Unit
                else -> error("资源释放失败：$closed")
            }
        }
        clientPublication = publication
    }
    private fun closeClientRuntime(current: ExtensionRuntime) {
        current.stop()
        var released = false
        for (step in 0 until 32) {
            clientSource?.let { source ->
                source.cancel()
                when (val closed = source.closeStep()) {
                    ExtensionFileSource.Status.OK -> clientSource = null
                    ExtensionFileSource.Status.WAIT -> Unit
                    else -> error("资源释放失败：$closed")
                }
            }
            if (current.closeStep()) { released = true; break }
        }
        if (!released || clientSource != null) throw CleanupPending("运行时与发布资源仍在释放，保留 owner")
        runtimeVersionLease?.close(); runtimeVersionLease = null; channelAdmission = null
        clientPublication = null
        runtime = null; runtimePackageKey = null; runtimeCommand = null; runtimeInstanceID = null; runtimeViewScope = null; runtimeViewIssue = null
        clientRootFD?.let { fd -> clientRootFD = null; Os.close(fd) }
    }

    suspend fun stepRuntime(): RuntimeState = onWorker {
        val current = checkNotNull(runtime) { "插件 Runtime 尚未启动" }
        // Native steps consume one wire frame each. Keep fragment progress on
        // this owner independent of the UI's 50 ms snapshot interval. The wall
        // budget is checked between calls; native CPU limits still apply.
        val deadline = SystemClock.elapsedRealtimeNanos() + 5_000_000L
        for (step in 0 until 32) {
            // The shared worker's timer cannot flush a logical channel's one
            // pending frame while this bounded Runtime batch owns the worker.
            parent?.multiplexer?.step()
            stepClientPublication()
            val phase = current.step()
            parent?.multiplexer?.step()
            if (phase != ExtensionRuntime.Phase.STARTING && phase != ExtensionRuntime.Phase.ACTIVE) {
                clientSource?.cancel(); stepClientPublication(); break
            }
            if (SystemClock.elapsedRealtimeNanos() >= deadline) break
        }
        runtimeState(current)
    }

    private fun runtimeState(current: ExtensionRuntime): RuntimeState {
        val key = checkNotNull(runtimePackageKey)
        val info = current.info()
        val views = if (runtimeViewScope != null && info.phase == ExtensionRuntime.Phase.ACTIVE &&
            runtimeViewIssue == null && !current.stopRequested) {
            try {
                current.viewsSnapshot()
            } catch (failure: ExtensionChannelException) {
                // An off-worker stop can revoke the grant after the ACTIVE observation.
                if (!current.stopRequested) throw failure
                null
            }
        } else null
        return RuntimeState(key.id, key.digest, checkNotNull(runtimeCommand), info, clientPublication, checkNotNull(runtimeInstanceID),
            views, runtimeViewScope?.first, runtimeViewScope?.second, runtimeViewIssue, current.stopRequested)
    }

    suspend fun closeRuntimeView(instanceID: String, viewHandle: String): RuntimeState = onWorker {
        val current = checkNotNull(runtime)
        check(runtimeInstanceID == instanceID && runtimeViewScope != null && current.info().phase == ExtensionRuntime.Phase.ACTIVE) { "视图不属于当前活跃实例" }
        current.closeView(viewHandle)
        runtimeState(current)
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
            closeClientRuntime(current)
            runtime = null; runtimePackageKey = null; runtimeCommand = null; runtimeInstanceID = null; runtimeViewScope = null; runtimeViewIssue = null
        }
        session?.let { current ->
            check(current.info().phase in setOf(ExtensionSession.Phase.ENDED, ExtensionSession.Phase.CLOSED)) {
                "不能释放仍活动或等待授权的插件 Session"
            }
            current.close()
            session = null; sessionPackageKey = null
            sessionVersionLease?.close(); sessionVersionLease = null; sessionApprovedSnapshot = false; channelAdmission = null
        }
    }

    suspend fun review(directory: File): Review = onWorker {
        checkOpen()
        val packageBytes = captureCheckedPackage(directory)
        try {
            val metadata = packageBytes.metadata()
            val key = Key(metadata.id, metadata.packageDigest)
            check(key !in records) { "相同 ID 与摘要的包已审查" }
            val value = packageReview(packageBytes)
            records[key] = Record(packageBytes, value, Source.UNKNOWN)
            value
        } catch (error: Throwable) {
            runCatching { packageBytes.close() }
            throw error
        }
    }

    private fun packageReview(packageBytes: ExtensionPackage): Review {
        val metadata = packageBytes.metadata()
        return Review(metadata.id, metadata.version, metadata.publisher, metadata.license,
            metadata.packageDigest, metadata.runtime, metadata.requires.toList(), metadata.optionalMissing.toList(),
            metadata.authorizationGranted, metadata.publisherAuthenticated,
            packageBytes.commands().map { Command(it.path, it.title, it.argsSchema, it.views, it.togglePanel) })
    }

    private fun captureCheckedPackage(directory: File): ExtensionPackage {
        require(directory.isAbsolute && directory.path.isNotBlank() && '\u0000' !in directory.path && directory.path.toByteArray().size <= 4096)
        val fd = Os.open(directory.path, OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        return try {
            ParcelFileDescriptor.dup(fd).use { captureCheckedPackage(it.fd) }
        } finally {
            Os.close(fd)
        }
    }
    private fun captureCheckedPackage(directoryFD: Int): ExtensionPackage {
        val packageBytes = ExtensionPackage.capture(directoryFD)
        try {
            var phase = packageBytes.step()
            var steps = 1
            while (phase == ExtensionPackage.Phase.CAPTURING && steps < CAPTURE_STEP_LIMIT) {
                phase = packageBytes.step(); steps++
            }
            check(phase == ExtensionPackage.Phase.CAPTURED) { "固定包快照失败：${packageBytes.info().code}" }
            check(packageBytes.check(hostJSON) == ExtensionPackage.Phase.CHECKED) {
                val code = packageBytes.info().code
                if (code == "LEGACY_PACKAGE_REINSTALL") "此包使用旧插件格式，请重新安装 Lua 版。"
                else "插件包与 Android Client 能力不兼容：$code"
            }
            return packageBytes
        } catch (error: Throwable) {
            runCatching { packageBytes.close() }
            throw error
        }
    }

    internal suspend fun inspectConfiguredChannelArchive(fd: ParcelFileDescriptor, descriptor: ByteArray, signature: ByteArray) =
        inspectSignedArchive(fd, descriptor, signature, byteArrayOf(), checkNotNull(shared.channelStore), ::configuredChannelTime)

    internal suspend fun installConfiguredChannelArchive(fd: ParcelFileDescriptor, descriptor: ByteArray, signature: ByteArray,
        expected: SignedArchiveReview) = installSignedArchive(fd, descriptor, signature, byteArrayOf(), expected,
            channelStore = checkNotNull(shared.channelStore), channelClock = ::configuredChannelTime)

    /** Preview a signed archive without registering a source or keeping an install.
     * The caller supplies a seekable read-only capture, not a provider stream.
     * Own the duplicate across queueing/cancellation and keep all writes private. */
    internal suspend fun inspectSignedArchive(borrowed: ParcelFileDescriptor, descriptor: ByteArray,
        signature: ByteArray, trustedPolicy: ByteArray, channelStore: PluginChannelPolicyStore? = null,
        channelClock: () -> ULong = ::channelClockSeconds): SignedArchiveReview {
        require(descriptor.size <= 65536 && signature.size == 64 && trustedPolicy.size <= 131072)
        val bytes = descriptor.copyOf(); val signed = signature.copyOf(); val policy = trustedPolicy.copyOf()
        val context = currentCoroutineContext()
        val owned = ParcelFileDescriptor.dup(borrowed.fileDescriptor)
        try {
            return onWorker {
                checkOpen()
                val channel = channelStore?.useCurrent(channelClock()) { it }
                val authenticated = channel?.authenticate(bytes, signed, channelClock())
                    ?: PluginReleaseSignature.authenticate(bytes, signed, policy)
                PluginReleaseArchive.open(owned, authenticated.proof, context::ensureActive).use { archive ->
                    val attributes = java.nio.file.attribute.PosixFilePermissions.asFileAttribute(
                        java.nio.file.attribute.PosixFilePermissions.fromString("rwx------"))
                    val stage = java.nio.file.Files.createTempDirectory(appContext.cacheDir.toPath(), "signed-release-review-", attributes).toFile()
                    try {
                        inspectArchiveContents(archive, authenticated, context::ensureActive, { member, content ->
                            val file = File(stage, member.path)
                            java.nio.file.Files.createDirectories(file.parentFile!!.toPath(), attributes)
                            val fd = Os.open(file.path, OsConstants.O_WRONLY or OsConstants.O_CREAT or OsConstants.O_EXCL or
                                OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 384)
                            try {
                                var at = 0
                                while (at < content.size) {
                                    context.ensureActive()
                                    val count = Os.write(fd, content, at, minOf(content.size - at, 16384))
                                    check(count > 0) { "无法写入签名包检查暂存文件" }; at += count
                                }
                                Os.fchmod(fd, if (member.executable) 320 else 256)
                            } finally { Os.close(fd) }
                        }, { captureCheckedPackage(stage) }).copy(channelStatementSHA256 = channel?.statementSHA256, channelGeneration = channel?.generation)
                    } finally {
                        // This is our just-created private preview tree. No
                        // package links are accepted and no install is published.
                        check(stage.deleteRecursively() || !stage.exists()) { "签名包检查暂存目录清理失败：${stage.path}" }
                    }
                }
            }
        } finally { owned.close() }
    }

    private fun inspectArchiveContents(archive: PluginReleaseArchive, authenticated: PluginReleaseSignature.Authenticated,
        checkpoint: () -> Unit, write: (PluginReleaseArchive.Member, ByteArray) -> Unit,
        capture: () -> ExtensionPackage): SignedArchiveReview {
        var manifest: ByteArray? = null
        archive.visit { member, content ->
            checkpoint()
            if (member.path == PluginReleaseDeclarations.manifestName) {
                require(content.size <= 65536) { "插件清单超过 64 KiB 上限" }
                manifest = content.copyOf()
            }
            write(member, content)
        }
        checkpoint()
        capture().use { packageBytes ->
            val review = packageReview(packageBytes)
            if (PluginReleaseSignature.Identity(review.id, review.version, review.publisher, review.digest) != authenticated.identity)
                throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.IDENTITY)
            val declarations = PluginReleaseDeclarations.decode(checkNotNull(manifest), packageBytes.metadata())
            return SignedArchiveReview(review, authenticated.proof, declarations)
        }
    }

    /** Explicit confirmation of a prior immutable review. Pre-publication work
     * observes the original caller's cancellation. The atomic publication and
     * its receipt run to completion; disposal never rolls back a published tree. */
    internal suspend fun installSignedArchive(borrowed: ParcelFileDescriptor, descriptor: ByteArray, signature: ByteArray,
        trustedPolicy: ByteArray, expected: SignedArchiveReview, installDirectory: File = versionDirectory,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) },
        channelStore: PluginChannelPolicyStore? = null, channelClock: () -> ULong = ::channelClockSeconds): InstalledRelease {
        require(descriptor.size <= 65536 && signature.size == 64 && trustedPolicy.size <= 131072)
        val raw = descriptor.copyOf(); val signed = signature.copyOf(); val policy = trustedPolicy.copyOf()
        val caller = currentCoroutineContext()
        val owned = ParcelFileDescriptor.dup(borrowed.fileDescriptor)
        try {
            return withContext(NonCancellable) { onWorker {
                caller.ensureActive()
                val channel = channelStore?.useCurrent(channelClock()) { it }
                requireChannelProof(channel, expected)
                val authenticated = channel?.authenticate(raw, signed, channelClock())
                    ?: PluginReleaseSignature.authenticate(raw, signed, policy)
                check(authenticated.proof == expected.proof) { "安装材料与已确认的发布证明不一致" }
                PluginReleaseArchive.open(owned, authenticated.proof, caller::ensureActive).use { archive ->
                    PluginReleaseStage.create(installDirectory, authenticated.proof.descriptorSHA256,
                        { phase -> caller.ensureActive(); checkpoint(phase) }, parentSync).use { stage ->
                        val fresh = inspectArchiveContents(archive, authenticated, caller::ensureActive,
                            { member, content -> stage.write("payload/${member.path}", content, member.executable) },
                            { stage.payload().use { captureCheckedPackage(it.fd) } }).copy(channelStatementSHA256 = channel?.statementSHA256, channelGeneration = channel?.generation)
                        check(fresh == expected) { "安装包或声明与已确认的完整预检不一致" }
                        stage.write("release.json", raw)
                        stage.write("release.sig", signed)
                        stage.write("policy.json", channel?.policyBytes() ?: policy)
                        if (channelStore != null) stage.write("channel-policy-required", channelStore.rootSHA256.toByteArray(Charsets.US_ASCII))
                        caller.ensureActive()
                        val publication = stage.publish { rename ->
                            if (channelStore == null) rename()
                            else {
                                val observed = channelClock()
                                channelStore.useCurrent(observed) { current ->
                                    requireChannelProof(current, expected)
                                    val now = channelClock()
                                    if (now < observed) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
                                    current.requireFresh(now)
                                    caller.ensureActive()
                                    rename()
                                }
                            }
                        }
                        InstalledRelease(fresh, publication)
                    }
                }
            } }
        } finally { owned.close() }
    }

    private fun channelClockSeconds(): ULong {
        val time = System.currentTimeMillis()
        if (time < 0) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_FRESHNESS)
        return (time / 1000).toULong()
    }

    private fun requireChannelProof(channel: PluginReleaseSignature.VerifiedChannelPolicy?, expected: SignedArchiveReview) {
        if (channel?.statementSHA256 != expected.channelStatementSHA256)
            throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
    }

    private fun fsIdentity(fd: java.io.FileDescriptor): PluginReleaseCatalog.Identity = Os.fstat(fd).let {
        PluginReleaseCatalog.Identity(it.st_dev, it.st_ino, it.st_uid)
    }
    private fun configuredChannelTime() = shared.channelClock?.invoke() ?: channelClockSeconds()

    private fun <T> withVersionChannel(version: PluginReleaseCatalog.Version,
        action: (PluginReleaseSignature.VerifiedChannelPolicy?) -> T): T {
        val root = version.channelRoot() ?: return action(null)
        val store = shared.channelStore
        if (store == null || store.rootSHA256 != root) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
        return store.useCurrent(configuredChannelTime(), action = action)
    }

    private fun <T> withChannelAdmission(version: PluginReleaseCatalog.Version?, record: Record, action: (ChannelAdmission?) -> T): T {
        if (version == null) return action(null)
        return withVersionChannel(version) { policy ->
            val admission = if (policy != null) {
                val descriptor = version.read("release.json", 65536)
                val authenticated = policy.authenticate(descriptor, version.read("release.sig", 64), configuredChannelTime())
                val metadata = record.packageBytes.metadata()
                check(authenticated.proof.descriptorSHA256 == version.releaseID &&
                    authenticated.identity == PluginReleaseSignature.Identity(metadata.id, metadata.version, metadata.publisher, metadata.packageDigest)) {
                    "已安装版本的签名身份已变化"
                }
                ChannelAdmission(policy.rootSHA256, authenticated.proof)
            } else null
            action(admission)
        }
    }

    private fun checkedVersion(owner: PluginReleaseCatalog, version: PluginReleaseCatalog.Version, policy: ByteArray,
        channel: PluginReleaseSignature.VerifiedChannelPolicy? = null): CheckedVersion {
        if (version.requiresChannelPolicy() && channel == null)
            throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
        val raw = version.read("release.json", 65536)
        check(PluginReleaseCatalog.sha(raw) == version.releaseID) { "版本发布描述已变化" }
        val signature = version.read("release.sig", 64)
        val authenticated = channel?.authenticate(raw, signature, configuredChannelTime())
            ?: PluginReleaseSignature.authenticate(raw, signature, policy)
        version.payload().use { payload ->
            val bytes = captureCheckedPackage(payload.fd)
            try {
                val metadata = bytes.metadata()
                val identity = PluginReleaseSignature.Identity(metadata.id, metadata.version, metadata.publisher, metadata.packageDigest)
                if (identity != authenticated.identity) throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.IDENTITY)
                val declarations = PluginReleaseDeclarations.decode(version.readPayload(PluginReleaseDeclarations.manifestName, 65536), metadata)
                owner.validateVersion(version, payload)
                return CheckedVersion(bytes, SignedArchiveReview(packageReview(bytes), authenticated.proof, declarations, channel?.statementSHA256, channel?.generation), fsIdentity(payload.fileDescriptor))
            } catch (failure: Throwable) { bytes.close(); throw failure }
        }
    }
    /** Caller-generated id is known before dispatch, so leaving the page can
     * cancel a preview whose fd result has not yet been delivered. */
    internal suspend fun previewInstalledVersion(id: String, releaseID: String, expectedRevision: String?, policy: ByteArray): VersionPreview {
        require(UUID.fromString(id).toString() == id && policy.size <= 131072)
        val caller = currentCoroutineContext(); val trusted = policy.copyOf()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive(); check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消上一次版本预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有本机版本目录")
            owner.use {
                val selection = owner.selection()
                if (selection.revision != expectedRevision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
                val candidate = owner.openVersion(releaseID)
                try {
                    candidate.pin()
                    withVersionChannel(candidate) { policy ->
                        val checked = checkedVersion(owner, candidate, trusted, policy)
                        val value = try { checked.value } finally { checked.packageBytes.close() }
                        val choice = selection.choices[value.review.id]
                        var previous: SignedArchiveReview? = null
                        var state = if (choice == null) PreviousVersionState.NONE else PreviousVersionState.UNKNOWN
                        if (choice != null) {
                            if (choice.releaseID == releaseID) { previous = value; state = PreviousVersionState.VERIFIED }
                            else try {
                                owner.openVersion(choice.releaseID).use { old ->
                                    val oldRoot = old.channelRoot()
                                    if (oldRoot != null && oldRoot != candidate.channelRoot())
                                        throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.CHANNEL_HISTORY)
                                    val earlier = checkedVersion(owner, old, trusted, policy)
                                    try {
                                        check(earlier.value.review.id == value.review.id)
                                        previous = earlier.value; state = PreviousVersionState.VERIFIED
                                    } finally { earlier.packageBytes.close() }
                                }
                            } catch (_: Exception) { previous = null; state = PreviousVersionState.UNKNOWN }
                        }
                        caller.ensureActive(); checkOpen()
                        if (owner.selection().revision != expectedRevision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
                        val view = VersionPreview(id, selection.revision, releaseID, value, previous, state, choice?.releaseID, choice?.enabled)
                        pendingVersion = PendingVersion(view, trusted, owner.rootIdentity, candidate, checked.payload)
                        view
                    }
                } catch (failure: Throwable) { candidate.close(); throw failure }
            }
        } }
    }
    internal fun cancelVersionPreview(id: String) {
        Handler(thread.looper).post {
            if (pendingVersion?.value?.id == id) { pendingVersion?.close(); pendingVersion = null }
        }
    }
    internal suspend fun confirmInstalledVersion(id: String, acknowledged: Boolean,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }): VersionSelectionResult {
        check(acknowledged) { "请先审阅完整声明并确认本次切换" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            val pending = pendingVersion?.takeIf { it.value.id == id } ?: error("版本预览已失效")
            pendingVersion = null
            pending.use {
                val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("本机版本目录已变化")
                owner.use {
                    check(owner.rootIdentity == pending.root) { "安装根目录已被替换" }
                    if (owner.selection().revision != pending.value.revision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
                    owner.openVersion(pending.value.releaseID).use { named -> check(named.identity == pending.version.identity) { "候选版本已被替换" } }
                    withVersionChannel(pending.version) { policy ->
                        requireChannelProof(policy, pending.value.candidate)
                        val fresh = checkedVersion(owner, pending.version, pending.policy, policy)
                        val sessionReferences = linkedMapOf<ExtensionController, ParcelFileDescriptor>()
                        val runtimeReferences = linkedMapOf<ExtensionController, ParcelFileDescriptor>()
                        try {
                            check(fresh.value == pending.value.candidate && fresh.payload == pending.payload) { "候选包或完整声明已变化" }
                            val key = Key(fresh.value.review.id, fresh.value.review.digest)
                            shared.controllers.forEach { run ->
                                if (run.sessionPackageKey == key && run.sessionVersionLease == null) sessionReferences[run] = pending.version.borrow()
                                if (run.runtimePackageKey == key && run.runtimeVersionLease == null) runtimeReferences[run] = pending.version.borrow()
                            }
                            val before = owner.observe()
                            val result = owner.change(fresh.value.review.id, pending.value.releaseID, true, pending.value.revision,
                                checkpoint = { phase ->
                                    caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen()
                                    if (phase == "before-rename") policy?.requireFresh(configuredChannelTime())
                                }, parentSync = parentSync)
                            invalidateVersionSources(fresh.value.review.id)
                            sessionReferences.forEach { (run, reference) -> run.sessionVersionLease = reference }; sessionReferences.clear()
                            runtimeReferences.forEach { (run, reference) -> run.runtimeVersionLease = reference }; runtimeReferences.clear()
                            val record = records[key] ?: Record(fresh.packageBytes, fresh.value.review, Source.UNKNOWN).also { records[key] = it }
                            record.managedReleaseID = pending.value.releaseID
                            record.selectionGeneration = result.selection.choices.getValue(fresh.value.review.id).generation
                            record.rootIdentity = pending.root; record.versionIdentity = pending.version.identity; record.payloadIdentity = fresh.payload
                            VersionSelectionResult(fresh.value.review.id, fresh.value.review,
                                before.copy(selection = result.selection, selectionIssue = null), result.durabilityConfirmed)
                        } finally {
                            sessionReferences.values.forEach { it.close() }; runtimeReferences.values.forEach { it.close() }
                            if (records[Key(fresh.value.review.id, fresh.value.review.digest)]?.packageBytes !== fresh.packageBytes) fresh.packageBytes.close()
                        }
                    }
                }
            }
        } }
    }
    internal suspend fun disableInstalledVersion(pluginID: String, expectedRevision: String?, acknowledged: Boolean,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }): VersionSelectionResult {
        check(acknowledged) { "请确认禁用后续新实例" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有活动选择目录")
            owner.use {
                val selected = owner.selection()
                if (selected.revision != expectedRevision) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.CONFLICT)
                val choice = selected.choices[pluginID] ?: error("此插件没有活动选择")
                val before = owner.observe()
                val result = owner.change(pluginID, choice.releaseID, false, expectedRevision,
                    checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, parentSync = parentSync)
                invalidateVersionSources(pluginID)
                VersionSelectionResult(pluginID, null, before.copy(selection = result.selection, selectionIssue = null), result.durabilityConfirmed)
            }
        } }
    }
    /** Capture Core packages on an independent worker, leaving an active pump
     * free. Only the in-memory ticket is installed on this Controller. */
    internal suspend fun previewPolicyReview(id: String, policy: ByteArray, checkpoint: (String) -> Unit = {}): PluginPolicyReview.Value {
        require(UUID.fromString(id).toString() == id && policy.size <= 131072)
        val trusted = policy.copyOf(); val caller = currentCoroutineContext()
        return withContext(NonCancellable) {
            onWorker {
                caller.ensureActive()
                check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消上一次预览或复核" }
                policyRequest = id
            }
            try {
                val inspector = newReleaseInspector()
                val review = try { inspector.onWorker {
                    caller.ensureActive()
                    PluginReleaseCatalog.open(versionDirectory).use { owner ->
                        PluginPolicyReview.observe(owner, id, trusted, { fd ->
                            inspector.captureCheckedPackage(fd).use { bytes ->
                                val metadata = bytes.metadata()
                                PluginReleaseSignature.Identity(metadata.id, metadata.version, metadata.publisher, metadata.packageDigest)
                            }
                        }, { caller.ensureActive(); checkpoint("policy-member"); caller.ensureActive() })
                    }
                } } finally { inspector.close() }
                onWorker {
                    caller.ensureActive(); check(policyRequest == id) { "策略复核已取消或过期" }
                    pendingPolicyReview = review; review.value
                }
            } catch (failure: Throwable) {
                if (!closed.get()) runCatching { onWorker { if (policyRequest == id) { policyRequest = null; pendingPolicyReview = null } } }
                throw failure
            }
        }
    }
    internal fun cancelPolicyReview(id: String) {
        Handler(thread.looper).post {
            if (policyRequest == id) { policyRequest = null; pendingPolicyReview = null }
        }
    }
    internal suspend fun applyPolicyReview(id: String, pluginIDs: List<String>, policy: ByteArray, acknowledged: Boolean,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }): PolicyDisableResult {
        check(acknowledged) { "请确认仅禁用已勾选的活动选择" }; require(policy.size <= 131072)
        val selectedIDs = pluginIDs.toList(); val trusted = policy.copyOf(); val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            val pending = pendingPolicyReview?.takeIf { it.value.id == id && policyRequest == id } ?: error("策略复核已失效")
            pendingPolicyReview = null; policyRequest = null
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("活动选择目录已变化")
            owner.use {
                val before = owner.observe()
                val result = pending.apply(owner, selectedIDs, trusted,
                    { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, parentSync)
                selectedIDs.forEach(::invalidateVersionSources)
                PolicyDisableResult(selectedIDs, before.copy(selection = result.selection, selectionIssue = null), result.durabilityConfirmed)
            }
        } }
    }
    /** Observation never resumes or cancels a saved deletion intent. */
    internal suspend fun pendingVersionRemoval(): PluginReleaseRemoval.Preview? = onWorker {
        PluginReleaseCatalog.open(versionDirectory)?.use { PluginReleaseRemoval.pending(it) }
    }
    internal suspend fun previewVersionRemoval(id: String, releaseID: String, expectedRevision: String?): PluginReleaseRemoval.Preview {
        require(UUID.fromString(id).toString() == id)
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive(); check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消上一次版本预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有本机版本目录")
            owner.use {
                val removal = PluginReleaseRemoval.prepare(owner, releaseID, expectedRevision)
                try {
                    caller.ensureActive(); checkOpen()
                    val view = removal.preview(id)
                    pendingRemoval = id to removal
                    view
                } catch (failure: Throwable) { removal.close(); throw failure }
            }
        } }
    }
    internal suspend fun unclassifiedDirectories(): List<PluginUnclassifiedDirectories.Entry> = onWorker {
        PluginReleaseCatalog.open(versionDirectory)?.use { PluginUnclassifiedDirectories.listing(it) } ?: emptyList()
    }
    internal suspend fun previewUnclassifiedRemoval(id: String, reference: PluginUnclassifiedDirectories.Reference): PluginReleaseRemoval.Preview {
        require(UUID.fromString(id).toString() == id)
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive(); check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消上一次版本预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有本机版本目录")
            owner.use {
                val removal = PluginReleaseRemoval.prepareUnclassified(owner, reference)
                try {
                    caller.ensureActive(); checkOpen()
                    val view = removal.preview(id); pendingRemoval = id to removal; view
                } catch (failure: Throwable) { removal.close(); throw failure }
            }
        } }
    }
    internal suspend fun installationMaintenance(): PluginInstallJournal.Observation = onWorker {
        PluginInstallJournal.observe(versionDirectory)
    }
    internal suspend fun retireInstallationRecord(reference: PluginInstallJournal.Reference, acknowledged: Boolean): Boolean {
        check(acknowledged) { "请确认只退役这条已解决的归属记录" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有安装归属目录")
            owner.use { PluginInstallJournal.retire(owner, reference) }
        } }
    }
    internal suspend fun previewStageRemoval(id: String, reference: PluginInstallJournal.Reference): PluginReleaseRemoval.Preview {
        require(UUID.fromString(id).toString() == id)
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive(); check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消上一次版本预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有安装归属目录")
            owner.use {
                val removal = PluginReleaseRemoval.prepareStage(owner, reference)
                try {
                    caller.ensureActive(); checkOpen()
                    val view = removal.preview(id); pendingRemoval = id to removal; view
                } catch (failure: Throwable) { removal.close(); throw failure }
            }
        } }
    }
    internal fun cancelRemovalPreview(id: String) {
        Handler(thread.looper).post {
            if (pendingRemoval?.first == id) { pendingRemoval?.second?.close(); pendingRemoval = null }
        }
    }
    internal suspend fun confirmVersionRemoval(id: String, acknowledged: Boolean,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }, acknowledgedUnclassified: Boolean = false): PluginReleaseRemoval.Result {
        check(acknowledged) { "请核对完整文件清单并单独确认删除" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            val removal = pendingRemoval?.takeIf { it.first == id }?.second ?: error("删除预览已失效")
            pendingRemoval = null
            removal.use {
                val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("本机版本目录已变化")
                owner.use {
                    val result = removal.confirm(owner,
                        confirmationCheckpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, rootSync = parentSync, acknowledgedUnclassified = acknowledgedUnclassified)
                    forgetRemovedVersion(result)
                }
            }
        } }
    }
    internal suspend fun resumeVersionRemoval(id: String, acknowledged: Boolean,
        checkpoint: (String) -> Unit = {}, parentSync: (java.io.FileDescriptor) -> Unit = { Os.fsync(it) }, acknowledgedUnclassified: Boolean = false): PluginReleaseRemoval.Result {
        check(acknowledged) { "请核对保留的删除意图并确认继续" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive()
            check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消当前预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有本机版本目录")
            owner.use {
                forgetRemovedVersion(PluginReleaseRemoval.resume(owner, id,
                    checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, sync = parentSync, acknowledgedUnclassified = acknowledgedUnclassified))
            }
        } }
    }
    internal suspend fun cancelPendingVersionRemoval(id: String, acknowledged: Boolean): Boolean {
        check(acknowledged) { "请确认撤销尚未开始移除的删除意图" }
        val caller = currentCoroutineContext()
        return withContext(NonCancellable) { onWorker {
            caller.ensureActive(); check(pendingVersion == null && pendingRemoval == null && policyRequest == null) { "请先取消当前预览" }
            val owner = PluginReleaseCatalog.open(versionDirectory) ?: error("没有本机版本目录")
            owner.use { PluginReleaseRemoval.cancelPending(owner, id) }
        } }
    }
    /** Disk effects are final even if registry cleanup fails. Existing instances
     * retain their original immutable package until their actual release. */
    private fun forgetRemovedVersion(result: PluginReleaseRemoval.Result): PluginReleaseRemoval.Result {
        if (!result.complete || result.stage != null || result.unclassified != null) return result
        var issue: String? = null
        val forgotten = mutableListOf<Pair<String, String>>()
        val iterator = records.iterator()
        while (iterator.hasNext()) {
            val (key, record) = iterator.next()
            if (record.managedReleaseID != result.releaseID) continue
            record.source = Source.UNKNOWN; record.approvalGeneration = null
            if (key == sessionPackageKey || key == runtimePackageKey) continue
            try {
                if (registeredDigests[key.id] == key.digest) {
                    check(registryOnWorker().remove(key.id, executor = false, expectedDigest = key.digest) == ExtensionRegistry.Status.OK)
                    registeredDigests.remove(key.id)
                }
                record.packageBytes.close(); iterator.remove(); forgotten += key.id to key.digest
            } catch (_: Exception) { issue = "磁盘回收已完成，但本次内存记录清理未完成；请停止当前插件并重新打开页面。" }
        }
        return result.copy(memoryIssue = issue, forgottenPackages = forgotten.toList())
    }
    private fun invalidateVersionSources(id: String) {
        managedIDs += id
        records.filterKeys { it.id == id }.values.forEach { it.source = Source.UNKNOWN; it.approvalGeneration = null }
    }
    private inline fun <T> managedAccess(body: (PluginReleaseCatalog?, Map<String, PluginReleaseCatalog.Choice>) -> T): T {
        val owner = PluginReleaseCatalog.open(versionDirectory)
        try {
            val observed = owner?.observe()
            if (observed != null) {
                if (observed.selection == null || observed.issues.isNotEmpty()) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.INVALID)
                managedIDs += observed.versions.map { it.pluginID }; managedIDs += observed.selection.choices.keys
                for ((id, choice) in observed.selection.choices) if (choice.enabled) {
                    if (observed.versions.none { it.pluginID == id && it.releaseID == choice.releaseID }) throw PluginReleaseCatalog.Rejected(PluginReleaseCatalog.Reason.INVALID)
                }
            }
            return body(owner, observed?.selection?.choices.orEmpty())
        } finally { owner?.close() }
    }
    private fun managedStatus(record: Record, owner: PluginReleaseCatalog?, choices: Map<String, PluginReleaseCatalog.Choice>, approval: Boolean): ExtensionRegistry.Status {
        if (record.review.id !in managedIDs) return ExtensionRegistry.Status.OK
        if (owner == null) return ExtensionRegistry.Status.STALE
        val choice = choices[record.review.id] ?: return ExtensionRegistry.Status.DENIED
        if (!choice.enabled) return ExtensionRegistry.Status.DENIED
        if (record.managedReleaseID != choice.releaseID || record.selectionGeneration != choice.generation || owner.rootIdentity != record.rootIdentity)
            return ExtensionRegistry.Status.STALE
        owner.openVersion(choice.releaseID).use { version ->
            if (version.identity != record.versionIdentity) return ExtensionRegistry.Status.STALE
            version.payload().use { if (fsIdentity(it.fileDescriptor) != record.payloadIdentity) return ExtensionRegistry.Status.STALE }
        }
        if (approval && record.approvalGeneration != choice.generation) return ExtensionRegistry.Status.STALE
        return ExtensionRegistry.Status.OK
    }

    /** Explicit public policy snapshot; no source approval or Session grant. */
    internal suspend fun verifyDirectorySignature(packageID: String, digest: String, descriptor: ByteArray,
        signature: ByteArray, trustedPolicy: ByteArray): PluginReleaseSignature.Proof {
        require(descriptor.size <= 65536 && signature.size == 64 && trustedPolicy.size <= 131072)
        val bytes = descriptor.copyOf(); val signed = signature.copyOf(); val policy = trustedPolicy.copyOf()
        return onWorker {
            checkOpen()
            val record = records[Key(packageID, digest)] ?: error("签名材料对应的固定包已失效")
            val metadata = record.packageBytes.metadata()
            PluginReleaseSignature.verify(bytes, signed, policy,
                PluginReleaseSignature.Identity(metadata.id, metadata.version, metadata.publisher, metadata.packageDigest))
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
        if (records.isEmpty()) return@onWorker Resolution(if (selectedID == null) ExtensionRegistry.Status.NOT_FOUND else ExtensionRegistry.Status.STALE)
        managedAccess { owner, choices ->
        val candidates = records.values.filter { record ->
            record.source != Source.DENIED && managedStatus(record, owner, choices, approval = false) == ExtensionRegistry.Status.OK && record.review.commands.any { overlaps(command, it.path) }
        }
        if (selectedID == null && candidates.size > 1) return@onWorker Resolution(ExtensionRegistry.Status.CONFLICT)
        val chosen = if (selectedID != null) records[Key(selectedID, selectedDigest!!)]
            ?: return@onWorker Resolution(ExtensionRegistry.Status.STALE)
        else candidates.firstOrNull()
        if (chosen == null) return@managedAccess Resolution(ExtensionRegistry.Status.NOT_FOUND)
        val admission = managedStatus(chosen, owner, choices, approval = chosen.source == Source.ALLOWED)
        if (admission != ExtensionRegistry.Status.OK) return@managedAccess Resolution(admission, chosen.review.id, chosen.review.digest)
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
                Command(contribution.path, contribution.title, contribution.argsSchema, contribution.views,
                    contribution.togglePanel))
        } finally {
            selectedPackage.close()
        }
        }
    }

    suspend fun decodeClientArguments(pluginID: String, packageDigest: String,
        command: String, arguments: List<String>): String? = onWorker {
        checkOpen()
        val record = records[Key(pluginID, packageDigest)] ?: error("插件包摘要已失效")
        managedAccess { owner, choices ->
            val admission = managedStatus(record, owner, choices, approval = true)
            if (admission != ExtensionRegistry.Status.OK) throw ExtensionRegistry.RegistryException(admission)
        }
        check(record.source == Source.ALLOWED && record.review.commands.any { it.path == command }) {
            "命令不属于已批准的固定插件包"
        }
        record.packageBytes.decodeCommandArguments(command, arguments)
    }

    private class CleanupPending(message: String) : IllegalStateException(message)
    suspend fun close() = withContext(NonCancellable) {
        val deadline = SystemClock.elapsedRealtime() + 3000
        while (true) {
            try { closeAttempt(); return@withContext }
            catch (failure: Exception) {
                val draining = failure is CleanupPending || (failure is com.hanxi.ghosterm.bridge.ExtensionChannelException && failure.status == 3)
                if (!draining || SystemClock.elapsedRealtime() >= deadline) throw failure
                delay(10)
            }
        }
    }
    private suspend fun closeAttempt() {
        closed.set(true)
        if (ownsLibrary) shared.closing.set(true)
        if (cleanupComplete.get()) return
        try {
            withContext(NonCancellable + dispatcher) {
                if (cleanupComplete.get()) return@withContext
                checkWorker()
                val dependents = shared.controllers.filter { it !== this@ExtensionController && (ownsLibrary || it.parent === this@ExtensionController) }
                // Logical owners must release before their physical connection.
                dependents.sortedBy { if (it.parent != null) 0 else 1 }.forEach { it.closeOnWorker() }
                closeOnWorker()
                if (ownsLibrary) {
                    policyRequest = null; pendingPolicyReview = null
                    pendingVersion?.close(); pendingVersion = null
                    pendingRemoval?.second?.close(); pendingRemoval = null
                    var failure: Throwable? = null
                    fun recordFailure(next: Throwable) {
                        val previous = failure
                        if (previous == null) failure = next else previous.addSuppressed(next)
                    }
                    registry?.let { current ->
                        val status = current.closeStatus(); registry = null
                        if (status != ExtensionRegistry.Status.OK) recordFailure(IllegalStateException("插件注册表关闭失败：$status"))
                    }
                    val iterator = records.iterator()
                    while (iterator.hasNext()) {
                        val record = iterator.next().value
                        try { record.packageBytes.close(); iterator.remove() }
                        catch (error: Throwable) { recordFailure(error); if (!record.packageBytes.hasOwner) iterator.remove() }
                    }
                    if (records.isNotEmpty()) {
                        val error = IllegalStateException("仍有插件包 owner 未释放；保留 controller worker 供重试")
                        failure?.let(error::addSuppressed); throw error
                    }
                    cleanupComplete.set(true)
                    failure?.let { throw it }
                }
            }
        } finally { if (ownsLibrary && cleanupComplete.get()) thread.quitSafely() }
    }

    private fun closeOnWorker() {
        checkWorker(); closed.set(true)
        if (cleanupComplete.get()) return
        runtime?.let { current -> closeClientRuntime(current) }
        session?.let { current ->
            current.close(); session = null; sessionPackageKey = null
            sessionVersionLease?.close(); sessionVersionLease = null; sessionApprovedSnapshot = false; channelAdmission = null
        }
        channel?.let { current -> current.close(); channel = null }
        multiplexer?.let { mux ->
            mux.step()
            val observation = multiplexState(mux)
            if (muxDrainDeadline == 0L) muxDrainDeadline = SystemClock.elapsedRealtime() + 1000
            if (!observation.transport.remoteEof && observation.info.error == 0 && SystemClock.elapsedRealtime() < muxDrainDeadline) {
                throw CleanupPending("等待共享执行端清理完成；请重试关闭")
            }
            finalMultiplexState = observation
            if (!mux.closeStep()) throw CleanupPending("共享连接仍在释放，保留 owner")
            finalMultiplexState = observation.copy(info = observation.info.copy(released = true))
            multiplexer = null
            muxPump?.let(shared.handler::removeCallbacks); muxPump = null
        }
        if (parent == null) {
            resourceStore?.let { current -> current.close(); resourceStore = null }
            if (resourceDirectory.exists() && !resourceDirectory.deleteRecursively()) throw IllegalStateException("插件资源私有目录清理失败")
        }
        if (!ownsLibrary) { cleanupComplete.set(true); shared.controllers.remove(this) }
    }

    private suspend fun setSource(id: String, digest: String, source: Source): ExtensionRegistry.Status = onWorker {
        checkOpen()
        val record = records[Key(id, digest)] ?: return@onWorker ExtensionRegistry.Status.STALE
        if (source == Source.ALLOWED) {
            managedAccess { owner, choices ->
            val admission = managedStatus(record, owner, choices, approval = false)
            if (admission != ExtensionRegistry.Status.OK) return@managedAccess admission
            val status = registryOnWorker().put(record.packageBytes, ExtensionRegistry.Source.ALLOWED,
                if (record.managedReleaseID != null) registeredDigests[id] else if (record.source == Source.ALLOWED) digest else null)
            if (status == ExtensionRegistry.Status.OK) {
                record.source = Source.ALLOWED; record.approvalGeneration = choices[id]?.generation; registeredDigests[id] = digest
            }
            status
            }
        } else {
            if (registeredDigests[id] == digest) {
                val status = registryOnWorker().remove(id, executor = false, expectedDigest = digest)
                if (status != ExtensionRegistry.Status.OK) return@onWorker status
                registeredDigests.remove(id)
            }
            record.source = Source.DENIED; record.approvalGeneration = null
            ExtensionRegistry.Status.DENIED
        }
    }

    private fun resourceStoreOnWorker(): ExtensionResourceStore {
        checkWorker()
        parent?.let { return it.resourceStoreOnWorker() }
        resourceStore?.let { return it }
        check(resourceDirectory.exists() || resourceDirectory.mkdirs()) { "无法创建插件资源私有目录" }
        Os.chmod(resourceDirectory.path, 448)
        val fd = Os.open(resourceDirectory.path,
            OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        return try {
            check(Os.fstat(fd).st_mode and OsConstants.S_IFMT == OsConstants.S_IFDIR) {
                "插件资源私有路径不是目录"
            }
            ParcelFileDescriptor.dup(fd).use { descriptor ->
                ExtensionResourceStore(descriptor.fd, totalBytes = RESOURCE_SESSION_BYTES * (if (multiplexer != null) 8 else 1),
                    resourceBytes = RESOURCE_BYTES, slots = 64).also { resourceStore = it }
            }
        } finally { Os.close(fd) }
    }

    private fun registryOnWorker(): ExtensionRegistry {
        checkOpen(); checkWorker()
        return registry ?: ExtensionRegistry().also { registry = it }
    }

    private fun checkOpen() { check(!closed.get() && !shared.closing.get()) { "插件控制器已关闭" }; checkWorker() }
    private fun checkWorker() { check(Thread.currentThread() === thread && Looper.myLooper() != Looper.getMainLooper()) }
    private suspend fun <T> onWorker(block: () -> T): T = withContext(dispatcher) {
        checkOpen(); parent?.multiplexer?.step()
        try { block() } finally { parent?.multiplexer?.step() }
    }

    private fun overlaps(lhs: String, rhs: String) = lhs == rhs || lhs.startsWith("$rhs ") || rhs.startsWith("$lhs ")

    companion object {
        private const val CAPTURE_STEP_LIMIT = 100_000
        private const val RESOURCE_BYTES = 1L * 1024 * 1024
        // Budget each Lua execution slice on the dedicated worker.
        private const val CLIENT_RUNTIME_CPU_MILLISECONDS = 100
        private const val RESOURCE_SESSION_BYTES = 8L * 1024 * 1024
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

package com.hanxi.ghosterm

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.ExtensionPackage
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.ExecutorCoroutineDispatcher
import kotlinx.coroutines.android.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/** App-side Client package owner. Native package and registry calls stay on one
 * non-UI worker. Reviewing never grants source trust or activates JavaScript. */
class ExtensionController(profile: HostProfile) {
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
        val digest: String, val commands: List<Command>, val sourceApproved: Boolean = false,
    )
    data class Resolution(
        val status: ExtensionRegistry.Status,
        val packageID: String? = null,
        val packageDigest: String? = null,
        val command: Command? = null,
    )

    private data class Key(val id: String, val digest: String)
    private data class Record(val packageBytes: ExtensionPackage, val review: Review, var source: Source)

    private val thread = HandlerThread("ghosterm-extension-controller", Thread.NORM_PRIORITY).apply { start() }
    private val dispatcher: ExecutorCoroutineDispatcher = Handler(thread.looper).asCoroutineDispatcher("ghosterm-extension-controller")
    private val hostJSON = profile.json()
    private val closed = AtomicBoolean(false)
    private val records = linkedMapOf<Key, Record>() // worker-confined
    private var registry: ExtensionRegistry? = null // worker-confined

    suspend fun review(directory: File): Review = onWorker {
        checkOpen()
        require(directory.isAbsolute && directory.path.isNotBlank() && '\u0000' !in directory.path && directory.path.toByteArray().size <= 4096)
        val fd = Os.open(directory.path, OsConstants.O_RDONLY or OsConstants.O_DIRECTORY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        val packageBytes = try { ExtensionPackage.capture(fd) } finally { Os.close(fd) }
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
                metadata.packageDigest, packageBytes.commands().map { Command(it.path, it.title, it.argsSchema, it.views) })
            records[key] = Record(packageBytes, value, Source.UNKNOWN)
            value
        } catch (error: Throwable) {
            runCatching { packageBytes.close() }
            throw error
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
        if (!closed.compareAndSet(false, true)) return
        withContext(dispatcher) {
            checkWorker()
            // Registry owns references to approved packages; release it first.
            registry?.let { runCatching { it.closeStatus() } }
            registry = null
            records.values.forEach { runCatching { it.packageBytes.close() } }
            records.clear()
        }
        dispatcher.close()
        thread.quitSafely()
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
        private fun validName(value: String) = value.isNotBlank() && '\u0000' !in value && value.toByteArray().size <= 128
        private fun validCommand(value: String): Boolean {
            if (value.isEmpty() || value.toByteArray().size > 519) return false
            val parts = value.split(' ')
            return parts.size in 1..8 && parts.all { it.length in 1..64 && it.matches(Regex("[a-z][a-z0-9-]*")) }
        }
    }
}

package com.hanxi.ghosterm.data

import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.ConfigBridge
import com.hanxi.ghosterm.bridge.CoreLibrary
import com.hanxi.ghosterm.domain.*
import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.charset.CodingErrorAction
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.Path
import java.nio.file.StandardCopyOption.*
import java.nio.file.StandardOpenOption.*
import java.nio.file.attribute.BasicFileAttributes
import java.security.MessageDigest
import java.util.UUID

private const val MAX_CONFIG = 1024 * 1024

data class ConfigRevision(val identity: String?, val digest: String)
data class ConfigSnapshot(val bytes: ByteArray, val revision: ConfigRevision, val hosts: List<Host>) {
    val text: String get() = bytes.toString(Charsets.UTF_8)
}
data class ConfigSaved(val snapshot: ConfigSnapshot, val warning: String? = null)

/** File operations are injectable so failure tests exercise the actual transaction. */
open class ConfigFileOps {
    open fun writeExclusive(path: Path, bytes: ByteArray) {
        val channel = FileChannel.open(path, CREATE_NEW, WRITE, NOFOLLOW_LINKS)
        try {
            channel.use {
                Os.chmod(path.toString(), 0x180) // 0600 before writing any config bytes
                val buffer = ByteBuffer.wrap(bytes)
                while (buffer.hasRemaining()) it.write(buffer)
                it.force(true)
            }
        } catch (error: Exception) {
            // Only remove a file successfully created by this invocation.
            try { Files.deleteIfExists(path) } catch (cleanup: Exception) { error.addSuppressed(cleanup) }
            throw error
        }
    }
    open fun replace(from: Path, to: Path) {
        Files.move(from, to, ATOMIC_MOVE, REPLACE_EXISTING)
    }
    open fun syncDirectory(directory: Path) {
        val fd = Os.open(directory.toString(), OsConstants.O_RDONLY or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC, 0)
        try {
            check(OsConstants.S_ISDIR(Os.fstat(fd).st_mode)) { "配置目录类型改变" }
            Os.fsync(fd)
        } finally { Os.close(fd) }
    }
}

/** All methods are blocking; controllers call them on Dispatchers.IO. */
class ConfigRepository(
    private val directory: File,
    private val ops: ConfigFileOps = ConfigFileOps(),
    private val editConfig: (ByteArray, String, Map<String, String?>, Boolean) -> ByteArray = { bytes, alias, changes, create ->
        ConfigBridge.edit(bytes, alias, changes, create)
    },
    private val renameConfig: (ByteArray, String, String) -> ByteArray = ConfigBridge::rename,
) {
    private val target = directory.toPath().resolve("config")
    companion object {
        private val processLock = Any()
        fun validate(bytes: ByteArray) {
            if (bytes.size > MAX_CONFIG) throw ConfigAccess("SSH 配置不能超过 1 MiB")
            try {
                Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes))
            } catch (error: Exception) { throw ConfigAccess("SSH 配置必须为 UTF-8 文本", error) }
            if (Regex("-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----").containsMatchIn(bytes.toString(Charsets.UTF_8)))
                throw ConfigAccess("SSH 配置中不能包含私钥材料")
            if (!CoreLibrary.roundTrip(bytes).contentEquals(bytes))
                throw ConfigAccess("配置未能完整解析，请检查引号和指令格式")
        }
    }
    private fun <T> locked(block: () -> T): T = synchronized(processLock) {
        Files.createDirectories(directory.toPath())
        if (Files.isSymbolicLink(directory.toPath())) throw ConfigAccess("配置目录不能是符号链接")
        Os.chmod(directory.path, 0x1c0) // 0700
        FileChannel.open(directory.toPath().resolve(".config.lock"), CREATE, WRITE, NOFOLLOW_LINKS).use { channel ->
            channel.lock().use { block() }
        }
    }
    private fun raw(): Pair<ByteArray, ConfigRevision> {
        if (!Files.exists(target, NOFOLLOW_LINKS))
            return byteArrayOf() to ConfigRevision(null, digest(byteArrayOf()))
        val before = Files.readAttributes(target, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
        if (!before.isRegularFile || before.size() > MAX_CONFIG) throw ConfigAccess("配置不是普通文件或超过 1 MiB")
        val bytes = FileChannel.open(target, READ, NOFOLLOW_LINKS).use { channel ->
            val output = java.io.ByteArrayOutputStream()
            val block = ByteBuffer.allocate(8192)
            while (channel.read(block) >= 0) {
                block.flip()
                if (output.size() + block.remaining() > MAX_CONFIG) throw ConfigAccess("配置超过 1 MiB")
                output.write(block.array(), 0, block.remaining()); block.clear()
            }
            output.toByteArray()
        }
        val after = Files.readAttributes(target, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
        if (before.fileKey() != after.fileKey() || before.size() != after.size() || before.lastModifiedTime() != after.lastModifiedTime())
            throw ConfigConflict()
        return bytes to ConfigRevision(after.fileKey().toString(), digest(bytes))
    }
    private fun digest(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    private fun snapshot(bytes: ByteArray, revision: ConfigRevision): ConfigSnapshot {
        validate(bytes)
        val hosts = ConfigBridge.hosts(bytes).filter { fields ->
            val name = fields[0]
            name.isNotBlank() && name.none { it in "*?![]" || it.isWhitespace() }
        }.distinctBy { it[0].lowercase() }.map {
            fun field(start: Int) = FieldSource(if (it[start] == "1") it[start + 1] else null,
                it[start + 2], it[start + 3], it[start + 4].toInt(), it[start + 5] == "1", it[start + 6] == "1")
            Host(it[0], it[1], it[2], it[3].toInt(), HostSource(it[4], it[5], it[6].toInt(), it[7].toInt(),
                it[8].toInt(), it[9] == "1", field(10), field(17), field(24)))
        }
        return ConfigSnapshot(bytes.copyOf(), revision, hosts)
    }
    fun load(): ConfigSnapshot = locked { val (bytes, revision) = raw(); snapshot(bytes, revision) }

    fun saveHost(previous: ConfigSnapshot, edit: HostEdit): ConfigSaved {
        val host = edit.originalAlias?.let { alias -> previous.hosts.singleOrNull { it.alias == alias } ?: throw ConfigConflict() }
        if (host != null && host.source?.editable != true)
            throw ConfigAccess(host.source?.editRestriction ?: "无法确定主机配置归属，请重新载入")
        if (previous.hosts.any { it.alias != host?.alias && it.alias.equals(edit.alias, ignoreCase = true) })
            throw ConfigAccess("该主机别名已存在")
        val changes = edit.changes(host)
        val renaming = host != null && host.alias != edit.alias
        if (host != null && changes.isEmpty() && !renaming) return ConfigSaved(load().also { if (it.revision != previous.revision) throw ConfigConflict() })
        val renamed = if (renaming) renameConfig(previous.bytes, host!!.alias, edit.alias) else previous.bytes
        if (renaming) {
            // The shared parser owns token semantics; this only checks other lines' bytes.
            val before = previous.text.split('\n').toMutableList()
            val after = renamed.toString(Charsets.UTF_8).split('\n').toMutableList()
            val header = host!!.source!!.line - 1
            if (header !in before.indices || header !in after.indices) throw ConfigAccess("找不到原 Host 行")
            before.removeAt(header); after.removeAt(header)
            if (before != after) throw ConfigAccess("重命名改变了其他配置行，已停止保存")
        }
        val candidate = if (host != null && changes.isEmpty()) renamed else editConfig(renamed, edit.alias, changes, host == null)
        // Refuse an edit if the shared editor touched unrelated lines. This is a
        // byte-preservation guard, not a second evaluator of ssh_config semantics.
        if (host != null) {
            val keys = changes.keys.joinToString("|") { Regex.escape(it) }
            val lines = Regex("(?im)^[\\t ]*(?:$keys)(?:[\\t =])[^\\n]*(?:\\n|$)")
            if (lines.replace(renamed.toString(Charsets.UTF_8), "") != lines.replace(candidate.toString(Charsets.UTF_8), ""))
                throw ConfigAccess("配置编辑改变了其他指令，已停止保存；请使用 SSH 配置页")
        } else if (!candidate.take(previous.bytes.size).toByteArray().contentEquals(previous.bytes)) {
            throw ConfigAccess("新增主机会改变已有配置，已停止保存")
        }
        val parsed = snapshot(candidate, previous.revision)
        val effective = parsed.hosts.firstOrNull { it.alias == edit.alias } ?: throw ConfigAccess("编辑后找不到主机")
        for (field in HostField.entries) {
            if (field.keyword !in changes) continue
            val requested = changes[field.keyword]
            val declared = field.source(effective) ?: throw ConfigAccess("编辑后无法确认字段来源")
            if (declared.explicitValue != requested)
                throw ConfigAccess("${field.label}未按请求写入或恢复继承，原配置未修改")
            if (requested != null && !declared.effectiveIsExplicit)
                throw ConfigAccess("${field.label}被更早的 Host 规则或 Include 覆盖，请在 SSH 配置页调整顺序")
        }
        return save(previous, candidate)
    }

    fun connectionIssues(snapshot: ConfigSnapshot, alias: String) =
        ConfigBridge.connectionIssues(snapshot.bytes, alias)

    fun save(previous: ConfigSnapshot, candidate: ByteArray): ConfigSaved {
        val proposed = candidate.copyOf()
        validate(proposed)
        // Validate all derived fields before committing, so malformed output never replaces the source.
        snapshot(proposed, previous.revision)
        return locked {
            val (current, revision) = raw()
            if (revision != previous.revision) throw ConfigConflict()
            if (current.contentEquals(proposed)) return@locked ConfigSaved(snapshot(current, revision))
            val temp = directory.toPath().resolve(".config-${UUID.randomUUID()}.new")
            var committed = false
            var staged = false
            try {
                if (revision.identity != null) {
                    val backup = directory.toPath().resolve("config.backup-${UUID.randomUUID()}")
                    try {
                        ops.writeExclusive(backup, current)
                        ops.syncDirectory(directory.toPath())
                    } catch (error: Exception) {
                        throw ConfigAccess("备份失败，原配置未修改", error)
                    }
                }
                ops.writeExclusive(temp, proposed)
                staged = true
                // Catch writes made during backup/staging, not only when the editor first opened.
                if (raw().second != revision) throw ConfigConflict()
                ops.replace(temp, target)
                staged = false
                committed = true
                val warning = try { ops.syncDirectory(directory.toPath()); null }
                    catch (_: Exception) { "配置已替换，但目录同步失败；请检查存储状态" }
                val (saved, savedRevision) = raw()
                if (!saved.contentEquals(proposed)) throw ConfigAccess("配置提交后又发生变化，请重新载入")
                ConfigSaved(snapshot(saved, savedRevision), warning)
            } catch (error: ConfigConflict) { throw error }
            catch (error: Exception) {
                if (error is ConfigAccess && !committed) throw error
                throw ConfigAccess(if (committed) "配置已提交，但重新读取失败；请重新载入" else "保存失败，原配置未修改", error)
            } finally { if (staged) Files.deleteIfExists(temp) }
        }
    }
}

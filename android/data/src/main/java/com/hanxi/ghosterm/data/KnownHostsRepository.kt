package com.hanxi.ghosterm.data

import android.os.Process
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.KnownHostRecord
import com.hanxi.ghosterm.bridge.KnownHostsBridge
import com.hanxi.ghosterm.bridge.KnownHostsUpdate
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.Base64

data class KnownHostItem(val id: String, val index: Int, val record: KnownHostRecord, val info: IdentityPublicInfo) {
    val removable: Boolean get() = record.recognized && record.marker.isEmpty() && !record.hashed
    val kind: String get() = when {
        !record.recognized -> "未识别记录 · 只读"
        record.marker == "@revoked" -> "已撤销的公钥 · 只读"
        record.marker == "@cert-authority" -> "证书机构规则 · 只读"
        record.marker.isNotEmpty() -> "不支持的规则 · 只读"
        record.hashed -> "哈希主机名 · 暂不支持匹配"
        info.fingerprint == null -> "普通记录 · 指纹不可用"
        else -> "已记录公钥"
    }
}

class KnownHostsSnapshot(bytes: ByteArray, val revision: String) {
    private val original = bytes.copyOf()
    val records: List<KnownHostItem> = KnownHostsBridge.records(original).mapIndexed { index, record ->
        val info = if (!record.recognized) IdentityPublicInfo(null, null, "记录格式无法识别，不能计算指纹。")
        else try {
            IdentityPublicInfoReader.readPublicKey(Base64.getDecoder().decode(record.keyBase64), record.keyType)
        } catch (_: IllegalArgumentException) { IdentityPublicInfo(null, null, "公钥编码损坏，不能计算指纹。") }
        KnownHostItem("$revision:${record.byteStart}:${record.byteEnd}", index, record, info)
    }
    internal fun expectedBytes() = original.copyOf()
}

/** Blocking file control plane. UI callers use Dispatchers.IO. Only the shared
 * C transaction writes: a Java lock cannot coordinate native TOFU writers. */
open class KnownHostsRepository(private val directory: File) {
    private val target = File(directory, "known_hosts")
    private fun missing(error: ErrnoException) = error.errno == OsConstants.ENOENT
    open fun load(): KnownHostsSnapshot {
        val parent = try { Os.lstat(directory.absolutePath) }
        catch (error: ErrnoException) { if (missing(error)) return snapshot(byteArrayOf(), "missing"); throw error }
        check(OsConstants.S_ISDIR(parent.st_mode)) { "主机验证目录不是普通目录，或是符号链接" }
        val fd = try { Os.open(target.absolutePath,
            OsConstants.O_RDONLY or OsConstants.O_CLOEXEC or OsConstants.O_NOFOLLOW or OsConstants.O_NONBLOCK, 0) }
        catch (error: ErrnoException) { if (missing(error)) return snapshot(byteArrayOf(), "missing"); throw error }
        val bytes: ByteArray
        val identity: String
        try {
            val before = Os.fstat(fd)
            check(OsConstants.S_ISREG(before.st_mode) && before.st_nlink == 1L && before.st_uid == Process.myUid()) {
                "主机验证记录必须是本应用的普通文件，不能是链接或设备"
            }
            check(before.st_size in 0..1024L * 1024) { "主机验证记录超过 1 MiB" }
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = Os.read(fd, buffer, 0, buffer.size)
                if (count == 0) break
                check(output.size() + count <= 1024 * 1024) { "主机验证记录超过 1 MiB" }
                output.write(buffer, 0, count)
            }
            bytes = output.toByteArray()
            val after = Os.fstat(fd)
            val named = Os.lstat(target.absolutePath)
            val parentAfter = Os.lstat(directory.absolutePath)
            check(before.st_dev == named.st_dev && before.st_ino == named.st_ino &&
                before.st_size == after.st_size && after.st_size == bytes.size.toLong() &&
                before.st_mtime == after.st_mtime && parent.st_dev == parentAfter.st_dev && parent.st_ino == parentAfter.st_ino) {
                "读取期间主机验证记录已变化，请重新载入"
            }
            identity = "${before.st_dev}:${before.st_ino}"
        } finally { Os.close(fd) }
        return snapshot(bytes, identity)
    }
    private fun snapshot(bytes: ByteArray, identity: String): KnownHostsSnapshot {
        val digest = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
        return KnownHostsSnapshot(bytes, "$identity:$digest")
    }
    open fun remove(snapshot: KnownHostsSnapshot, id: String): KnownHostsUpdate {
        val entry = snapshot.records.singleOrNull { it.id == id }
        check(entry != null && entry.removable) { "此记录只读，或已不属于当前快照" }
        return KnownHostsBridge.remove(target.absolutePath, snapshot.expectedBytes(), entry.index)
    }
}

/** Keep control characters visible without changing the stored/source identity. */
fun String.trustDisplay(): String = buildString {
    for (c in this@trustDisplay) {
        if (Character.isISOControl(c) || Character.getType(c) == Character.FORMAT.toInt())
            append("\\u%04x".format(c.code))
        else append(c)
    }
}

package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import android.system.StructStat
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.security.MessageDigest
import java.text.Normalizer
import java.util.Locale
import java.util.zip.CRC32
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/** Strict zip-v1: seek a borrowed read-only regular file, own only its duplicate.
 * Parsing and inflate are bounded; neither opens member paths nor executes code.
 * Callers stage privately and publish only after visit returns and Core agrees. */
internal class PluginReleaseArchive private constructor(
    private val owner: ParcelFileDescriptor,
    private val proof: PluginReleaseSignature.Proof,
    private val checkpoint: () -> Unit,
) : AutoCloseable {
    enum class Failure { ARTIFACT, FILE, LAYOUT, PATH, BUDGET, CONTENT, CHANGED, CLOSED }
    class Rejected(val reason: Failure) : IllegalArgumentException(when (reason) {
        Failure.ARTIFACT -> "ZIP 字节数或摘要与签名描述不一致。"
        Failure.FILE -> "ZIP 需要可定位的只读普通文件句柄。"
        Failure.PATH -> "ZIP 含非法路径、重复成员、大小写冲突或非普通文件。"
        Failure.BUDGET -> "ZIP 条目或解压大小超过上限。"
        Failure.CONTENT -> "ZIP 内容、CRC 或压缩流校验失败。"
        Failure.CHANGED -> "ZIP 在读取期间发生变化，请重新选择。"
        Failure.CLOSED -> "ZIP 检查已结束，请重新选择。"
        Failure.LAYOUT -> "ZIP 结构不受支持或存在未声明范围。"
    })
    data class Member internal constructor(val path: String, val bytes: Int, val executable: Boolean)
    private data class Entry(val member: Member, val offset: Long, val compressed: Int, val method: Int, val crc: Long)
    private val fd get() = owner.fileDescriptor
    private val before = Os.fstat(fd)
    private val length = before.st_size
    private var closed = false
    private var visiting = false
    private var entries: List<Entry> = emptyList()
    val members: List<Member> get() { active(); return entries.map { it.member } }

    private fun active() { ensure(!closed, Failure.CLOSED); checkpoint() }
    private fun unchanged() { ensure(stamp(before) == stamp(Os.fstat(fd)), Failure.CHANGED) }
    private fun read(at: Long, bytes: ByteArray, offset: Int = 0, count: Int = bytes.size) {
        active()
        ensure(at >= 0 && at <= length && count >= 0 && count.toLong() <= length - at, Failure.LAYOUT)
        var received = 0
        while (received < count) {
            active()
            val size = Os.pread(fd, bytes, offset + received, count - received, at + received)
            ensure(size > 0, Failure.CHANGED)
            received += size
        }
    }
    private fun span(at: Long, count: Int) = ByteArray(count).also { read(at, it) }
    private fun u16(at: Long): Int { val b = span(at, 2); return (b[0].toInt() and 255) or ((b[1].toInt() and 255) shl 8) }
    private fun u32(at: Long): Long {
        val b = span(at, 4)
        return (0..3).fold(0L) { value, i -> value or ((b[i].toLong() and 255) shl (i * 8)) }
    }
    private fun artifact() {
        ensure(length in 22..LIMIT && length == proof.artifactBytes, Failure.ARTIFACT)
        val hash = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(16384)
        var at = 0L
        while (at < length) {
            val count = minOf(buffer.size.toLong(), length - at).toInt()
            read(at, buffer, count = count); hash.update(buffer, 0, count); at += count
        }
        ensure(hex(hash.digest()) == proof.artifactSHA256, Failure.ARTIFACT)
        unchanged()
    }
    private fun inspect(): List<Entry> {
        ensure(OsConstants.S_ISREG(before.st_mode) &&
            FileDescriptorFlags.status(fd) and OsConstants.O_ACCMODE == OsConstants.O_RDONLY, Failure.FILE)
        artifact()
        val end = length - 22
        ensure(u32(end) == 0x06054b50L && u16(end + 4) == 0 && u16(end + 6) == 0 && u16(end + 20) == 0, Failure.LAYOUT)
        val count = u16(end + 10); val centralBytes = u32(end + 12); val central = u32(end + 16)
        ensure(count in 1..1024 && u16(end + 8) == count && centralBytes >= count * 46L &&
            centralBytes <= count * (46L + 4096) && central + centralBytes == end, Failure.BUDGET)
        data class Header(val entry: Entry, val flags: Int, val name: ByteArray)
        val headers = mutableListOf<Header>(); val names = mutableSetOf<String>()
        var at = central; var total = 0L
        repeat(count) {
            ensure(u32(at) == 0x02014b50L, Failure.LAYOUT)
            val flags = u16(at + 8); val method = u16(at + 10); val crc = u32(at + 16)
            val compressed = u32(at + 20); val size = u32(at + 24); val nameSize = u16(at + 28)
            val external = u32(at + 38); val local = u32(at + 42)
            ensure(nameSize in 1..4096 && u16(at + 30) == 0 && u16(at + 32) == 0 && u16(at + 34) == 0, Failure.LAYOUT)
            val raw = span(at + 46, nameSize)
            val name = try { Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(raw)).toString() }
                catch (_: java.nio.charset.CharacterCodingException) { reject(Failure.PATH) }
            ensure(flags and 0x800.inv() == 0 && (flags and 0x800 != 0 || raw.all { it >= 0 }) && validPath(name) &&
                names.add(name.lowercase(Locale.ROOT)) && external and 16L == 0L &&
                ((external ushr 16).toInt() and OsConstants.S_IFMT) in setOf(0, OsConstants.S_IFREG), Failure.PATH)
            ensure(method in setOf(0, 8) && size <= 16777216 && compressed <= LIMIT, Failure.BUDGET)
            total += size; ensure(total <= 67108864, Failure.BUDGET)
            headers += Header(Entry(Member(name, size.toInt(), (external ushr 16) and 73L != 0L),
                local, compressed.toInt(), method, crc), flags, raw)
            at += 46 + nameSize
        }
        ensure(at == end, Failure.LAYOUT)
        for (name in names) {
            val parts = name.split('/')
            for (depth in 1 until parts.size) ensure(parts.take(depth).joinToString("/") !in names, Failure.PATH)
        }
        var cursor = 0L
        val result = headers.sortedBy { it.entry.offset }.map { header ->
            val entry = header.entry; val offset = entry.offset
            ensure(offset == cursor && u32(offset) == 0x04034b50L && u16(offset + 6) == header.flags &&
                u16(offset + 8) == entry.method && u32(offset + 14) == entry.crc &&
                u32(offset + 18) == entry.compressed.toLong() && u32(offset + 22) == entry.member.bytes.toLong() &&
                u16(offset + 26) == header.name.size && u16(offset + 28) == 0 &&
                span(offset + 30, header.name.size).contentEquals(header.name), Failure.LAYOUT)
            val start = offset + 30 + header.name.size
            cursor = start + entry.compressed
            ensure(cursor <= central, Failure.LAYOUT)
            entry.copy(offset = start)
        }
        ensure(cursor == central, Failure.LAYOUT)
        unchanged()
        return result
    }

    /** Complete validation is the successful return, never an individual callback.
     * At most one member (16 MiB + sentinel) is inflated in memory at a time. */
    fun visit(consume: (Member, ByteArray) -> Unit) {
        active(); check(!visiting); visiting = true
        try {
            unchanged()
            for (entry in entries) {
                active()
                val content = contents(entry)
                consume(entry.member, content)
                active(); unchanged()
            }
            artifact()
        } finally { visiting = false }
    }
    private fun contents(entry: Entry): ByteArray {
        val output = ByteArray(entry.member.bytes + 1)
        if (entry.method == 0) {
            ensure(entry.compressed == entry.member.bytes, Failure.CONTENT)
            read(entry.offset, output, count = entry.member.bytes)
        } else {
            val inflater = Inflater(true)
            try {
                val input = ByteArray(16384); var loaded = 0; var produced = 0
                while (!inflater.finished()) {
                    active()
                    if (inflater.needsInput()) {
                        val count = minOf(input.size, entry.compressed - loaded)
                        ensure(count > 0, Failure.CONTENT)
                        read(entry.offset + loaded, input, count = count)
                        inflater.setInput(input, 0, count); loaded += count
                    }
                    val count = try { inflater.inflate(output, produced, output.size - produced) }
                        catch (_: DataFormatException) { reject(Failure.CONTENT) }
                    produced += count
                    ensure(produced <= entry.member.bytes && !inflater.needsDictionary() &&
                        (count > 0 || inflater.needsInput() || inflater.finished()), Failure.CONTENT)
                }
                ensure(inflater.bytesRead == entry.compressed.toLong() && inflater.bytesWritten == entry.member.bytes.toLong(), Failure.CONTENT)
            } finally { inflater.end() }
        }
        val crc = CRC32().apply { update(output, 0, entry.member.bytes) }.value
        ensure(crc == entry.crc, Failure.CONTENT)
        return output.copyOf(entry.member.bytes)
    }
    override fun close() { if (!closed) { closed = true; owner.close() } }

    companion object {
        const val LIMIT = 75497472L
        fun open(borrowed: ParcelFileDescriptor, proof: PluginReleaseSignature.Proof, checkpoint: () -> Unit = {}): PluginReleaseArchive {
            val duplicate = ParcelFileDescriptor.dup(borrowed.fileDescriptor)
            try {
                val archive = PluginReleaseArchive(duplicate, proof, checkpoint)
                archive.entries = archive.inspect()
                return archive
            } catch (failure: Throwable) { duplicate.close(); throw failure }
        }
        private fun stamp(v: StructStat) = listOf(v.st_dev, v.st_ino, v.st_mode.toLong(), v.st_uid.toLong(), v.st_nlink,
            v.st_size, v.st_mtim.tv_sec, v.st_mtim.tv_nsec, v.st_ctim.tv_sec, v.st_ctim.tv_nsec)
        private fun hex(bytes: ByteArray) = bytes.joinToString("") { "%02x".format(it.toInt() and 255) }
        private fun validPath(value: String): Boolean {
            if (value.isEmpty() || value.toByteArray(Charsets.UTF_8).size > 4096 || value.startsWith('/') ||
                '\\' in value || ':' in value || Normalizer.normalize(value, Normalizer.Form.NFC) != value ||
                value.codePoints().anyMatch { it < 32 || it == 127 }) return false
            val parts = value.split('/')
            return parts.size <= 32 && parts.all { it.isNotEmpty() && it != "." && it != ".." && !it.endsWith('.') && !it.endsWith(' ') }
        }
        private fun reject(reason: Failure): Nothing = throw Rejected(reason)
        private fun ensure(value: Boolean, reason: Failure) { if (!value) reject(reason) }
    }
}

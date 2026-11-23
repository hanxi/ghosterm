package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.CancellationException
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID
import java.util.zip.CRC32
import java.util.zip.Deflater

@RunWith(AndroidJUnit4::class)
class PluginReleaseArchiveTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private data class Item(val name: String, val bytes: ByteArray = byteArrayOf(1, 2, 3), val method: Int = 0,
        val mode: Long = 0x81240000L, val flags: Int = 0x800, val declared: Int = bytes.size,
        val compressed: ByteArray? = null, val crc: Long = CRC32().apply { update(bytes) }.value)
    private fun deflate(bytes: ByteArray): ByteArray {
        val zip = Deflater(6, true)
        try {
            zip.setInput(bytes); zip.finish()
            val result = ByteArrayOutputStream(); val buffer = ByteArray(4096)
            while (!zip.finished()) { val n = zip.deflate(buffer); check(n > 0); result.write(buffer, 0, n) }
            return result.toByteArray()
        } finally { zip.end() }
    }
    private fun put(bytes: ByteArray, at: Int, value: Long, count: Int = 4) {
        repeat(count) { bytes[at + it] = (value ushr (it * 8)).toByte() }
    }
    private fun int(bytes: ByteArray, at: Int) = (0..3).fold(0L) { v, i -> v or ((bytes[at + i].toLong() and 255) shl (8 * i)) }.toInt()
    private fun zip(vararg items: Item): ByteArray {
        val local = ByteArrayOutputStream(); val central = ByteArrayOutputStream()
        for (item in items) {
            val raw = item.name.toByteArray(); val compressed = item.compressed ?: if (item.method == 8) deflate(item.bytes) else item.bytes
            val head = ByteArray(30); put(head, 0, 0x04034b50); put(head, 4, 20, 2); put(head, 6, item.flags.toLong(), 2)
            put(head, 8, item.method.toLong(), 2); put(head, 14, item.crc); put(head, 18, compressed.size.toLong())
            put(head, 22, item.declared.toLong()); put(head, 26, raw.size.toLong(), 2)
            val directory = ByteArray(46); put(directory, 0, 0x02014b50); put(directory, 4, 0x314, 2); put(directory, 6, 20, 2)
            put(directory, 8, item.flags.toLong(), 2); put(directory, 10, item.method.toLong(), 2); put(directory, 16, item.crc)
            put(directory, 20, compressed.size.toLong()); put(directory, 24, item.declared.toLong()); put(directory, 28, raw.size.toLong(), 2)
            put(directory, 38, item.mode); put(directory, 42, local.size().toLong())
            local.write(head); local.write(raw); local.write(compressed); central.write(directory); central.write(raw)
        }
        val end = ByteArray(22); put(end, 0, 0x06054b50); put(end, 8, items.size.toLong(), 2); put(end, 10, items.size.toLong(), 2)
        put(end, 12, central.size().toLong()); put(end, 16, local.size().toLong())
        return local.toByteArray() + central.toByteArray() + end
    }
    private fun proof(bytes: ByteArray) = PluginReleaseSignature.Proof("org.ghosterm.fixture", "a".repeat(64), "fixture", "b".repeat(64),
        "fixture", "c".repeat(64), "d".repeat(64), MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }, bytes.size.toLong())
    private fun withFile(bytes: ByteArray, block: (File, ParcelFileDescriptor) -> Unit) {
        val file = File(context.cacheDir, "archive-test-${UUID.randomUUID()}").apply { writeBytes(bytes) }
        try { ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { block(file, it) } }
        finally { assertTrue(file.delete() || !file.exists()) }
    }
    private fun rejected(bytes: ByteArray, reason: PluginReleaseArchive.Failure? = null, consume: Boolean = true) {
        withFile(bytes) { _, fd ->
            try {
                PluginReleaseArchive.open(fd, proof(bytes)).use { archive -> if (consume) archive.visit { _, _ -> } }
                fail("invalid archive accepted")
            } catch (expected: PluginReleaseArchive.Rejected) { if (reason != null) assertEquals(reason, expected.reason) }
        }
    }
    @Test fun storedDeflatedEmptyUnicodeAndExecutableMembersDecodeExactly() {
        val items = arrayOf(Item("README", "public bytes".toByteArray()), Item("client/入口.js", ByteArray(50000) { it.toByte() }, 8, 0x816d0000L),
            Item("empty-stored", byteArrayOf()), Item("empty-deflated", byteArrayOf(), 8))
        val bytes = zip(*items)
        withFile(bytes) { _, fd -> PluginReleaseArchive.open(fd, proof(bytes)).use { archive ->
            assertEquals(items.map { it.name }, archive.members.map { it.path })
            var index = 0
            archive.visit { member, body ->
                assertEquals(items[index].name, member.path); assertEquals(items[index].declared, member.bytes)
                assertEquals(index == 1, member.executable); assertArrayEquals(items[index++].bytes, body)
            }
            assertEquals(items.size, index)
        } }
    }
    @Test fun artifactBytesAndDigestMustMatchBeforeAnyMember() {
        val bytes = zip(Item("a"))
        withFile(bytes) { _, fd ->
            for (wrong in listOf(proof(bytes).copy(artifactBytes = bytes.size + 1L), proof(bytes).copy(artifactSHA256 = "0".repeat(64)))) {
                try { PluginReleaseArchive.open(fd, wrong).use { }; fail("wrong signed artifact accepted") }
                catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.ARTIFACT, e.reason) }
            }
        }
    }
    @Test fun archiveLimitRejectsSparseOversizeBeforeReadingIt() {
        withFile(byteArrayOf()) { file, fd ->
            RandomAccessFile(file, "rw").use { it.setLength(PluginReleaseArchive.LIMIT + 1) }
            var reads = 0
            try { PluginReleaseArchive.open(fd, proof(byteArrayOf()).copy(artifactBytes = PluginReleaseArchive.LIMIT + 1)) { reads++ }.use { }; fail("oversize accepted") }
            catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.ARTIFACT, e.reason) }
            assertEquals("oversize guard must reject before reading/hash allocation", 0, reads)
        }
    }
    @Test fun onlyReadOnlyRegularDescriptorsAreAccepted() {
        val bytes = zip(Item("a"))
        withFile(bytes) { file, _ ->
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_WRITE).use { fd ->
                try { PluginReleaseArchive.open(fd, proof(bytes)).use { }; fail("writable archive accepted") }
                catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.FILE, e.reason) }
            }
        }
        val pipe = ParcelFileDescriptor.createPipe()
        try {
            try { PluginReleaseArchive.open(pipe[0], proof(bytes)).use { }; fail("pipe accepted as seekable archive") }
            catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.FILE, e.reason) }
        } finally { pipe.forEach { it.close() } }
    }
    @Test fun canonicalPathsRejectTraversalControlsAndNormalizationAliases() {
        for (path in listOf("/a", "../a", "a/../b", "a/./b", "a//b", "a/", "a\\b", "C:a", "a.", "a ", "a\n", "a\u0000", "e\u0301", (1..33).joinToString("/") { "a" }, "a".repeat(4097)))
            rejected(zip(Item(path)), consume = false)
        val bytes = zip(Item("a")); val central = int(bytes, bytes.size - 6)
        bytes[30] = 0xff.toByte(); bytes[central + 46] = 0xff.toByte()
        rejected(bytes, PluginReleaseArchive.Failure.PATH)
    }
    @Test fun duplicateCaseFoldAndFileDirectoryConflictsAreRejected() {
        for (items in listOf(arrayOf(Item("a"), Item("a")), arrayOf(Item("A"), Item("a")),
            arrayOf(Item("dir"), Item("DIR/file")), arrayOf(Item("x/FILE"), Item("X/file/child"))))
            rejected(zip(*items), PluginReleaseArchive.Failure.PATH)
    }
    @Test fun symlinksDevicesDirectoriesAndUnknownFlagsAreRejected() {
        for (mode in listOf(0xa1ff0000L, 0x21ff0000L, 0x41ff0000L, 0x81240010L)) rejected(zip(Item("a", mode = mode)), PluginReleaseArchive.Failure.PATH)
        for (flags in listOf(1, 8, 0x1000)) rejected(zip(Item("a", flags = flags)), PluginReleaseArchive.Failure.PATH)
        rejected(zip(Item("中", flags = 0)), PluginReleaseArchive.Failure.PATH)
    }
    @Test fun centralDirectoryRequiresExactSingleDiskAndNoExtrasOrComment() {
        val bytes = zip(Item("a")); val end = bytes.size - 22; val central = int(bytes, end + 16)
        for ((at, size) in listOf(end + 4 to 2, end + 6 to 2, end + 20 to 2, central + 30 to 2, central + 32 to 2, central + 34 to 2)) {
            val broken = bytes.copyOf(); put(broken, at, 1, size); rejected(broken)
        }
        rejected(bytes + byteArrayOf(0)); rejected(bytes.copyOf(bytes.size - 1)); rejected(byteArrayOf())
    }
    @Test fun localAndCentralHeadersMustAgreeExactly() {
        val bytes = zip(Item("a")); val central = int(bytes, bytes.size - 6)
        for ((at, size) in listOf(0 to 4, 6 to 2, 8 to 2, 14 to 4, 18 to 4, 22 to 4, 26 to 2, 28 to 2, 30 to 1)) {
            val broken = bytes.copyOf(); broken[at] = (broken[at].toInt() xor 1).toByte(); rejected(broken)
        }
        val outOfRange = bytes.copyOf(); put(outOfRange, central + 42, 0xffffffffL); rejected(outOfRange)
    }
    @Test fun payloadRangesHaveNoPrefixGapOverlapOrTrailingBytes() {
        val bytes = zip(Item("a"), Item("b")); val end = bytes.size - 22; val central = int(bytes, end + 16)
        for (offset in listOf(1L, 2L, 0xffffffffL)) {
            val broken = bytes.copyOf(); put(broken, central + 42, offset); rejected(broken)
        }
        val broken = bytes.copyOf(); put(broken, central + 47 + 42, 0); rejected(broken)
        val prefixed = byteArrayOf(0) + bytes
        put(prefixed, end + 1 + 16, central + 1L)
        put(prefixed, central + 1 + 42, 1)
        put(prefixed, central + 1 + 47 + 42, int(bytes, central + 47 + 42) + 1L)
        rejected(prefixed)
        rejected(byteArrayOf(0) + bytes)
    }
    @Test fun memberCountSingleAndTotalBudgetsPrecedeInflateAllocation() {
        val bytes = zip(Item("a")); val end = bytes.size - 22
        for (count in listOf(0L, 1025L, 65535L)) {
            val broken = bytes.copyOf(); put(broken, end + 8, count, 2); put(broken, end + 10, count, 2); rejected(broken, PluginReleaseArchive.Failure.BUDGET)
        }
        rejected(zip(), PluginReleaseArchive.Failure.BUDGET)
        rejected(zip(*(1..1025).map { Item("$it", byteArrayOf()) }.toTypedArray()), PluginReleaseArchive.Failure.BUDGET)
        rejected(zip(Item("a", declared = 16777217)), PluginReleaseArchive.Failure.BUDGET)
        rejected(zip(*(1..5).map { Item("$it", declared = 16777216) }.toTypedArray()), PluginReleaseArchive.Failure.BUDGET)
        rejected(zip(Item("a", method = 99)), PluginReleaseArchive.Failure.BUDGET)
    }
    @Test fun storedLengthAndCrcAreVerifiedBeforeDeliveringMember() {
        rejected(zip(Item("a", declared = 2)), PluginReleaseArchive.Failure.CONTENT)
        for (method in listOf(0, 8)) rejected(zip(Item("a", method = method, crc = 1)), PluginReleaseArchive.Failure.CONTENT)
    }
    @Test fun deflateRequiresExactInputOutputEndAndNoDictionary() {
        val original = ByteArray(1000) { 1 }; val stream = deflate(original)
        for (compressed in listOf(stream + byteArrayOf(0), stream.copyOf(stream.size - 1), byteArrayOf(0xff.toByte()), byteArrayOf()))
            rejected(zip(Item("a", original, 8, compressed = compressed)), PluginReleaseArchive.Failure.CONTENT)
        rejected(zip(Item("a", original, 8, declared = 2)), PluginReleaseArchive.Failure.CONTENT)
        val paddingCRC = CRC32().apply { update(ByteArray(2)) }.value
        rejected(zip(Item("a", byteArrayOf(), 8, declared = 2, crc = paddingCRC)), PluginReleaseArchive.Failure.CONTENT)
    }
    @Test fun duplicatedDescriptorsCloseWithoutConsumingBorrowedOffset() {
        val bytes = zip(Item("a"))
        withFile(bytes) { _, fd ->
            Os.lseek(fd.fileDescriptor, 7, OsConstants.SEEK_SET)
            val before = File("/proc/self/fd").list()!!.size
            repeat(32) { PluginReleaseArchive.open(fd, proof(bytes)).use { archive -> archive.visit { _, _ -> } } }
            assertTrue("archive duplicate leaked", File("/proc/self/fd").list()!!.size <= before + 2)
            assertEquals(7L, Os.lseek(fd.fileDescriptor, 0, OsConstants.SEEK_CUR)); assertEquals(bytes.size.toLong(), Os.fstat(fd.fileDescriptor).st_size)
        }
    }
    @Test fun modifiedArchiveIsRejectedBeforeCallbackAndBetweenMembers() {
        val bytes = zip(Item("a"), Item("b"))
        for (during in listOf(false, true)) withFile(bytes) { file, fd ->
            PluginReleaseArchive.open(fd, proof(bytes)).use { archive ->
                var callbacks = 0
                if (!during) file.appendBytes(byteArrayOf(0))
                try {
                    archive.visit { _, _ -> callbacks++; if (during) file.appendBytes(byteArrayOf(0)) }
                    fail("changed archive accepted")
                } catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.CHANGED, e.reason) }
                assertEquals(if (during) 1 else 0, callbacks)
            }
        }
    }
    @Test fun checkpointsCancelAndReleaseEvenFailedOpens() {
        val bytes = zip(Item("a", ByteArray(50000), 8))
        withFile(bytes) { _, fd ->
            val before = File("/proc/self/fd").list()!!.size
            repeat(20) {
                var calls = 0
                try { PluginReleaseArchive.open(fd, proof(bytes)) { if (++calls == 3) throw CancellationException("test cancellation") }.use { }; fail("cancellation ignored") }
                catch (_: CancellationException) { assertEquals(3, calls) }
            }
            assertTrue("failed open leaked", File("/proc/self/fd").list()!!.size <= before + 2)
        }
    }
    @Test fun closedArchiveCannotReadOrPublishMembers() {
        val bytes = zip(Item("a"))
        withFile(bytes) { _, fd ->
            val archive = PluginReleaseArchive.open(fd, proof(bytes)); archive.close(); archive.close()
            try { archive.members; fail("closed archive exposed usable members") }
            catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.CLOSED, e.reason) }
            try { archive.visit { _, _ -> fail("closed member delivered") }; fail("closed archive accepted") }
            catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.CLOSED, e.reason) }
        }
    }
    @Test fun publicSignaturesBindBothStoredAndDeflatedLuaArchiveBytes() {
        val fixture = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/archive.json").bufferedReader().use { it.readText() })
        val vectors = fixture.getJSONArray("vectors")
        for (index in 0..1) {
            val vector = vectors.getJSONObject(index)
            val authenticated = PluginReleaseSignature.authenticate(vector.getString("descriptor").toByteArray(),
                Base64.getDecoder().decode(vector.getString("signature")), fixture.getString("policy").toByteArray())
            val bytes = Base64.getDecoder().decode(vector.getString("archive"))
            withFile(bytes) { _, fd -> PluginReleaseArchive.open(fd, authenticated.proof).use { archive ->
                assertTrue(archive.members.any { it.path == "manifest.json" }); archive.visit { _, _ -> }
            } }
        }
    }
}

package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginRemovalFilesTest {
    private val id = "b".repeat(64)
    @Test fun actualPersistentIdentityMetadataSurvivesRenameAndBorrowedDescriptorsRemainOpen() {
        val root = PluginInstallTestFiles.root()
        try { LocalPublicationFile.directory(root, "").use { parent ->
            val version = File(root, id).apply { check(mkdir()); Os.chmod(path, 320) }
            LocalPublicationFile.childDirectory(parent.fd, id).use { fd ->
                val before = PluginInstallFiles.identity(fd.fd)
                assertEquals(4, before.size); assertEquals("persistent filesystem identity support is required on this test filesystem", 0L, before[0])
                assertTrue(before[1] in 1..2); assertTrue(before[3] in 0..999999999)
                val target = ".removing-${UUID.randomUUID()}"
                assertEquals(0, PluginInstallFiles.detach(parent.fd, fd.fd, id, target))
                assertFalse(version.exists()); assertTrue(File(root, target).isDirectory)
                assertArrayEquals(before, PluginInstallFiles.identity(fd.fd))
                assertEquals(Os.fstat(fd.fileDescriptor).st_ino, Os.lstat(File(root, target).path).st_ino)
            }
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun identityRejectsBadPublicWritableAndNonFileDescriptors() {
        val root = PluginInstallTestFiles.root()
        try {
            assertEquals(OsConstants.EBADF.toLong(), PluginInstallFiles.identity(-1)[0])
            val file = File(root, "file").apply { writeText("preserve") }
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_WRITE).use { fd ->
                Os.fchmod(fd.fileDescriptor, 384)
                assertEquals(OsConstants.EINVAL.toLong(), PluginInstallFiles.identity(fd.fd)[0])
                assertEquals(8L, Os.fstat(fd.fileDescriptor).st_size)
            }
            Os.chmod(file.path, 292)
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd -> assertEquals(OsConstants.EINVAL.toLong(), PluginInstallFiles.identity(fd.fd)[0]) }
            Os.chmod(file.path, 256)
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd -> assertEquals(0L, PluginInstallFiles.identity(fd.fd)[0]) }
            ParcelFileDescriptor.createPipe().let { pipe -> try { assertEquals(OsConstants.EINVAL.toLong(), PluginInstallFiles.identity(pipe[0].fd)[0]) } finally { pipe.forEach { it.close() } } }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun detachNeverOverwritesEmptyOrPopulatedTombstone() {
        for (populated in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try { LocalPublicationFile.directory(root, "").use { parent ->
                File(root, id).apply { check(mkdir()); Os.chmod(path, 320) }
                val target = ".removing-${UUID.randomUUID()}"
                val old = File(root, target).apply { check(mkdir()); if (populated) File(this, "preserve").writeText("old") }
                val inode = Os.lstat(old.path).st_ino
                LocalPublicationFile.childDirectory(parent.fd, id).use { fd -> assertEquals(OsConstants.EEXIST, PluginInstallFiles.detach(parent.fd, fd.fd, id, target)) }
                assertEquals(inode, Os.lstat(old.path).st_ino); assertTrue(File(root, id).isDirectory)
                if (populated) assertEquals("old", File(old, "preserve").readText())
            } } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun detachedSourceReplacementAndWritableDirectoryAreRejected() {
        val root = PluginInstallTestFiles.root()
        try { LocalPublicationFile.directory(root, "").use { parent ->
            val source = File(root, id).apply { check(mkdir()) }
            LocalPublicationFile.childDirectory(parent.fd, id).use { fd ->
                val target = ".removing-${UUID.randomUUID()}"
                assertEquals(OsConstants.EINVAL, PluginInstallFiles.detach(parent.fd, fd.fd, id, target))
                Os.fchmod(fd.fileDescriptor, 320); check(source.renameTo(File(root, "preserved-original")))
                check(source.mkdir()); Os.chmod(source.path, 320)
                assertEquals(OsConstants.EINVAL, PluginInstallFiles.detach(parent.fd, fd.fd, id, target))
                assertTrue(source.isDirectory); assertFalse(File(root, target).exists())
            }
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun noncanonicalComponentsNeverReachDetachSyscall() {
        val root = PluginInstallTestFiles.root()
        try { LocalPublicationFile.directory(root, "").use { parent ->
            File(root, id).apply { check(mkdir()); Os.chmod(path, 320) }
            LocalPublicationFile.childDirectory(parent.fd, id).use { fd ->
                for (target in listOf("../x", ".removing-" + "-".repeat(36), ".removing-" + "a".repeat(36))) {
                    val failure = runCatching { PluginInstallFiles.detach(parent.fd, fd.fd, id, target) }
                    assertTrue("invalid name reached a successful syscall", failure.exceptionOrNull() is IllegalArgumentException || failure.getOrNull() == OsConstants.EINVAL)
                }
                assertTrue(File(root, id).isDirectory); assertTrue(Os.fstat(fd.fileDescriptor).st_ino > 0)
            }
        } } finally { PluginInstallTestFiles.clean(root) }
    }
}

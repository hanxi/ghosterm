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
class PluginInstallFilesTest {
    private val id = "b".repeat(64)
    private val name get() = ".prepare-${UUID.randomUUID()}"
    @Test fun realDirectoryRenameIsAtomicAndNeverOverwritesExistingEmptySlot() {
        val root = PluginInstallTestFiles.root(); val source = name
        try { LocalPublicationFile.directory(root, "").use { parent ->
            assertEquals(0, LocalPublicationFile.mkdir(parent.fd, source))
            LocalPublicationFile.childDirectory(parent.fd, source).use { directory ->
                Os.fchmod(directory.fileDescriptor, 320)
                assertTrue(File(root, id).mkdir()); val original = Os.lstat(File(root, id).path)
                assertEquals(OsConstants.EEXIST, PluginInstallFiles.publish(parent.fd, directory.fd, source, id))
                assertEquals(original.st_ino, Os.lstat(File(root, id).path).st_ino)
                assertTrue(File(root, id).delete())
                val before = Os.fstat(directory.fileDescriptor)
                assertEquals(0, PluginInstallFiles.publish(parent.fd, directory.fd, source, id))
                assertFalse(File(root, source).exists()); assertEquals(before.st_ino, Os.lstat(File(root, id).path).st_ino)
                assertEquals(before.st_ino, Os.fstat(directory.fileDescriptor).st_ino)
            }
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun writableDirectoryAndRegularFileDescriptorsAreRejected() {
        val root = PluginInstallTestFiles.root(); val source = name
        try { LocalPublicationFile.directory(root, "").use { parent ->
            assertEquals(0, LocalPublicationFile.mkdir(parent.fd, source))
            LocalPublicationFile.childDirectory(parent.fd, source).use { directory ->
                assertEquals(OsConstants.EINVAL, PluginInstallFiles.publish(parent.fd, directory.fd, source, id))
            }
            val file = File(root, "ordinary").apply { writeText("preserve") }
            Os.chmod(file.path, 256)
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                assertEquals(OsConstants.EINVAL, PluginInstallFiles.lock(fd.fd))
                assertEquals(OsConstants.EINVAL, PluginInstallFiles.publish(parent.fd, fd.fd, source, id))
                assertEquals(file.length(), Os.fstat(fd.fileDescriptor).st_size)
            }
            assertFalse(File(root, id).exists())
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun sourceReplacementAndSymlinkCannotRedirectBorrowedDirectory() {
        for (link in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val source = name
            try { LocalPublicationFile.directory(root, "").use { parent ->
                assertEquals(0, LocalPublicationFile.mkdir(parent.fd, source))
                LocalPublicationFile.childDirectory(parent.fd, source).use { directory ->
                    Os.fchmod(directory.fileDescriptor, 320)
                    check(File(root, source).renameTo(File(root, "original")))
                    if (link) Os.symlink("original", File(root, source).path) else { check(File(root, source).mkdir()); Os.chmod(File(root, source).path, 320) }
                    assertEquals(OsConstants.EINVAL, PluginInstallFiles.publish(parent.fd, directory.fd, source, id))
                    assertTrue(File(root, "original").isDirectory); assertFalse(File(root, id).exists())
                }
            } } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun locksAreBoundToIndependentOpenDescriptionsAndReleaseOnClose() {
        val root = PluginInstallTestFiles.root()
        try {
            LocalPublicationFile.directory(root, "").use { first ->
                assertEquals(0, PluginInstallFiles.lock(first.fd))
                LocalPublicationFile.directory(root, "").use { second -> assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(second.fd)) }
                assertTrue(Os.fstat(first.fileDescriptor).st_ino > 0)
            }
            LocalPublicationFile.directory(root, "").use { assertEquals(0, PluginInstallFiles.lock(it.fd)) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun invalidComponentsDoNotReachFilesystemAndNeverConsumeCallerFds() {
        val root = PluginInstallTestFiles.root()
        try { LocalPublicationFile.directory(root, "").use { fd ->
            Os.chmod(root.path, 493)
            assertEquals(OsConstants.EINVAL, PluginInstallFiles.lock(fd.fd))
            Os.chmod(root.path, 448)
            for (source in listOf("../x", "x/y", ".", "", ".prepare-\u0000")) {
                try { PluginInstallFiles.publish(fd.fd, fd.fd, source, id); fail("invalid component accepted") } catch (_: IllegalArgumentException) { }
            }
            try { PluginInstallFiles.publish(fd.fd, fd.fd, name, "../${id}"); fail("invalid target accepted") } catch (_: IllegalArgumentException) { }
            assertTrue(root.list()!!.isEmpty()); assertTrue(Os.fstat(fd.fileDescriptor).st_ino > 0)
        } } finally { PluginInstallTestFiles.clean(root) }
    }
}

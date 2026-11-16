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

@RunWith(AndroidJUnit4::class)
class PluginVersionLeaseTest {
    private fun child(root: File) = File(root, "version").also { check(it.mkdir()); Os.chmod(it.path, 320) }
    @Test fun sharedReferenceSurvivesOriginalOwnerUntilLastBorrowedFdCloses() {
        val root = PluginInstallTestFiles.root()
        try {
            child(root)
            val original = LocalPublicationFile.directory(root, "version")
            assertEquals(0, PluginInstallFiles.pin(original.fd)); val borrowed = ParcelFileDescriptor.dup(original.fileDescriptor)
            try {
                original.close()
                LocalPublicationFile.directory(root, "version").use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
                assertTrue(Os.fstat(borrowed.fileDescriptor).st_ino > 0)
            } finally { borrowed.close(); original.close() }
            LocalPublicationFile.directory(root, "version").use { assertEquals(0, PluginInstallFiles.lock(it.fd)) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun independentSharedReadersCoexistAndExclusiveOwnerWaitsForBoth() {
        val root = PluginInstallTestFiles.root()
        try {
            child(root)
            val first = LocalPublicationFile.directory(root, "version"); val second = LocalPublicationFile.directory(root, "version")
            try {
                assertEquals(0, PluginInstallFiles.pin(first.fd)); assertEquals(0, PluginInstallFiles.pin(second.fd))
                first.close()
                LocalPublicationFile.directory(root, "version").use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
            } finally { first.close(); second.close() }
            LocalPublicationFile.directory(root, "version").use { assertEquals(0, PluginInstallFiles.lock(it.fd)) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun writableDirectoryRegularFileAndBadFdReturnErrnoWithoutConsumingBorrowedFd() {
        val root = PluginInstallTestFiles.root()
        try {
            LocalPublicationFile.directory(root, "").use { assertEquals(OsConstants.EINVAL, PluginInstallFiles.pin(it.fd)); assertTrue(Os.fstat(it.fileDescriptor).st_ino > 0) }
            val file = File(root, "file").apply { writeText("public"); Os.chmod(path, 256) }
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { assertEquals(OsConstants.EINVAL, PluginInstallFiles.pin(it.fd)); assertEquals(6, Os.fstat(it.fileDescriptor).st_size.toInt()) }
            assertEquals(OsConstants.EBADF, PluginInstallFiles.pin(-1))
        } finally { PluginInstallTestFiles.clean(root) }
    }
}
